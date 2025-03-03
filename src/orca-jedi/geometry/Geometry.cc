/*
 * (C) British Crown Copyright 2024 Met Office
 */

#include "orca-jedi/geometry/Geometry.h"

#include "atlas/field/Field.h"
#include "atlas/field/FieldSet.h"
#include "atlas/functionspace/StructuredColumns.h"
#include "atlas/mesh.h"
#include "atlas/meshgenerator.h"
#include "atlas/parallel/mpi/mpi.h"

#include "eckit/mpi/Comm.h"
#include "eckit/config/Configuration.h"
#include "eckit/exception/Exceptions.h"
#include "eckit/system/ResourceUsage.h"

#include "oops/base/Variables.h"
#include "oops/util/DateTime.h"
#include "oops/util/Logger.h"

namespace {
/// \brief Construct an atlas grid given a string containing either a grid name
///        or a path to a grid specification yaml configuration file.
/// \param[in]     grid_specification  string containing the path/name.
/// \return        constructed atlas grid object.
atlas::Grid construct_grid_from_name(std::string grid_specification) {
  auto grid_name = grid_specification;
  eckit::PathName grid_spec_path(grid_specification);

  std::vector<std::string> orca_grid_names;
  for (auto && orca_type : std::vector<std::string>{"F", "T", "U", "V", "W"}) {
    for (auto && resolution : std::vector<std::string>{"1", "2", "025", "12"}) {
      orca_grid_names.emplace_back("ORCA" + resolution + "_" + orca_type);
      orca_grid_names.emplace_back("eORCA" + resolution + "_" + orca_type);
    }
  }
  auto grid = atlas::Grid();
  if (std::find(std::begin(orca_grid_names), std::end(orca_grid_names), grid_name)
      != std::end(orca_grid_names)) {
    grid = atlas::Grid{grid_name};
  } else if (grid_spec_path.exists()) {
    grid = atlas::Grid{atlas::Grid::Spec{grid_spec_path}};
  } else {
    std::stringstream err_stream;
    err_stream << "orcamodel::Geometry:: grid  \"" << grid_specification
               << "\" " << " is neither a valid named grid,"
               << " nor a path to a grid specification. " << std::endl;
    throw eckit::BadValue(err_stream.str(), Here());
  }
  return grid;
}
}  // namespace

namespace orcamodel {

oops::Variables orcaVariableFactory(const eckit::Configuration & config) {
  OrcaGeometryParameters params;
  params.validateAndDeserialize(config);

  oops::Variables variables{};
  std::vector<std::string> names{};
  for (const NemoFieldParameters& nemoVariable :
        params.nemoFields.value()) {
    std::string name = nemoVariable.name.value();
    if (std::find(names.begin(), names.end(), name) == names.end()) {
      names.emplace_back(name);
      variables.push_back(oops::Variable(name));
    }
  }

  return variables;
}

// -----------------------------------------------------------------------------
Geometry::Geometry(const eckit::Configuration & config,
                   const eckit::mpi::Comm & comm) :
                      comm_(comm), vars_(orcaVariableFactory(config)),
                      n_levels_(config.getInt("number levels")),
                      eckit_timer_(new eckit::Timer("Geometry(ORCA): ", oops::Log::trace()))
{
    eckit_timer_->start();
    log_status();
    params_.validateAndDeserialize(config);

    grid_ = construct_grid_from_name(params_.gridName.value());

    int64_t halo = params_.sourceMeshHalo.value();
    if ( ( (params_.partitioner.value() == "serial") || (comm.size() == 1) )
         && (halo > 0) ) {
      halo = 0;
      oops::Log::info() << "Warning: forcing halo = 0"
                        << " as settings imply all processors have all data" << std::endl;
    }
    auto meshgen_config = grid_.meshgenerator()
                          | atlas::option::halo(halo);

    atlas::MeshGenerator meshgen(meshgen_config);
    log_status();
    auto partitioner_config = grid_.partitioner();
    partitioner_config.set("type",
        params_.partitioner.value());
    partitioner_ = atlas::grid::Partitioner(partitioner_config);
    log_status();
    mesh_ = meshgen.generate(grid_, partitioner_);
    log_status();
    funcSpace_ = atlas::functionspace::NodeColumns(
        mesh_, atlas::option::halo(halo));
    log_status();
}

// -----------------------------------------------------------------------------
Geometry::~Geometry() {}

const std::string Geometry::nemo_var_name(const std::string std_name) const {
  for (const auto & nemoField : params_.nemoFields.value()) {
    if (std_name == nemoField.name.value()) return nemoField.nemoName.value();
  }
  std::stringstream err_stream;
  err_stream << "orcamodel::Geometry::nemo_var_name variable name \" ";
  err_stream << "\" " << std_name << " not recognised. " << std::endl;
  throw eckit::BadValue(err_stream.str(), Here());
}

// -----------------------------------------------------------------------------
/// \brief Give the number of levels for each provided level - surface variables
///        have 1 level, volumetric variables have "number levels" levels.
/// \param[in]     vars  variables to check.
/// \return        vector of number of levels in each variable.
std::vector<size_t> Geometry::variableSizes(const oops::Variables & vars) const
{
  std::vector<size_t> varSizes(vars.size());
  std::fill(varSizes.begin(), varSizes.end(), 0);

  auto nemoFields = params_.nemoFields.value();

  for (size_t i=0; i < vars.size(); ++i) {
    for (const auto & nemoField : nemoFields) {
      if (nemoField.name.value() == vars[i].name()) {
        if (nemoField.modelSpace.value() == "surface") {
          varSizes[i] = 1;
        } else {
          varSizes[i] = n_levels_;
        }
      }
    }
    if (varSizes[i] == 0) {
      std::stringstream err_stream;
      err_stream << "orcamodel::Geometry::variableSizes variable name \" ";
      err_stream << "\" " << vars[i].name() << " not recognised. " << std::endl;
      throw eckit::BadValue(err_stream.str(), Here());
    }
  }
  return varSizes;
}

void Geometry::latlon(std::vector<double> & lats, std::vector<double> & lons,
    const bool halo) const {
  const auto lonlat = atlas::array::make_view<double, 2>(funcSpace_.lonlat());
  const auto ghosts = atlas::array::make_view<int32_t, 1>(
      mesh_.nodes().ghost());
  const auto haloDistance = atlas::array::make_view<int32_t, 1>(
      mesh_.nodes().halo());
  auto isRequired = [&](const size_t nodeElem) {
    if (halo) {
      return !ghosts(nodeElem) || (haloDistance(nodeElem) > 0);
    }
    return !ghosts(nodeElem);
  };
  const size_t npts = funcSpace_.size();
  for (size_t nodeElem = 0; nodeElem < npts; ++nodeElem) {
    if (isRequired(nodeElem)) {
      lons.emplace_back(lonlat(nodeElem, 0));
      lats.emplace_back(lonlat(nodeElem, 1));
    }
  }
}

// -----------------------------------------------------------------------------
/// \brief Give the space of nemo field for each variable - surface, volume or
///         vertical. at the moment we need this distinction to read 3D depth
///         data from a 1D array
/// \param[in]     vars  variables to check.
/// \return        vector of variable Nemo model spaces.
std::vector<std::string> Geometry::variableNemoSpaces(
    const oops::Variables & vars) const
{
  std::vector<std::string> varNemoSpaces(vars.size(), "");

  auto nemoFields = params_.nemoFields.value();

  for (size_t i=0; i < vars.size(); ++i) {
    for (const auto & nemoField : nemoFields) {
      if (nemoField.name.value() == vars[i].name()) {
        if (nemoField.modelSpace.value() == "surface" ||
            nemoField.modelSpace.value() == "volume" ||
            nemoField.modelSpace.value() == "vertical" ) {
          varNemoSpaces[i] = nemoField.modelSpace.value();
        } else {
            std::stringstream err_stream;
            err_stream << "orcamodel::Geometry::variableNemoSpaces modelSpace"
                       << " \"" << nemoField.modelSpace.value()
                       << "\" not recognised for field \""
                       << nemoField.name.value() << "\"." << std::endl;
            throw eckit::BadValue(err_stream.str(), Here());
        }
      }
    }
    if (varNemoSpaces[i] == "") {
      std::stringstream err_stream;
      err_stream << "orcamodel::Geometry::variableSizes variable name \"";
      err_stream << vars[i] << "\" not available in the state. ";
      err_stream << "Either add this state variable to the model ";
      err_stream << "configuration or remove the corresponding obs variable";
      err_stream << " from the filters configuration." << std::endl;
      throw eckit::BadValue(err_stream.str(), Here());
    }
  }
  return varNemoSpaces;
}

const oops::Variables & Geometry::variables() const {
  return vars_;
}

/// \brief Check if a variable's data is a member of a type (e.g if it can be
///        sourced from the background file, variance file, or MDT file).
/// \param[in]     variable_name  Name of variable.
/// \param[in]     variable_type  Type of variable.
/// \return        Boolean for membership.
const bool Geometry::variable_in_variable_type(std::string variable_name,
  std::string variable_type) const {
  auto nemoFields = params_.nemoFields.value();
  for (const auto & nemoField : nemoFields) {
    if (nemoField.name.value() == variable_name) {
      std::string type = nemoField.variableType.value();
      return type == variable_type;
    }
  }

  std::stringstream err_stream;
  err_stream << "orcamodel::Geometry::variable_in_variable_type variable name ";
  err_stream << "\"" << variable_name << "\" not recognised. " << std::endl;
  throw eckit::BadValue(err_stream.str(), Here());
}

/// \brief Data type of the atlas field holding the named variable data.
/// \param[in]     variable_name  Name of variable.
/// \return        orcamodel::FieldDType enum.
FieldDType Geometry::fieldPrecision(std::string variable_name) const {
  auto nemoFields = params_.nemoFields.value();
  for (const auto & nemoField : nemoFields) {
    if (nemoField.name.value() == variable_name) {
       return nemoField.fieldPrecision.value();
    }
  }

  std::stringstream err_stream;
  err_stream << "orcamodel::Geometry::fieldPrecision variable name ";
  err_stream << "\"" << variable_name << "\" not recognised. " << std::endl;
  throw eckit::BadValue(err_stream.str(), Here());
}

void Geometry::print(std::ostream & os) const {
  os << "Not Implemented";
}

void Geometry::log_status() const {
  oops::Log::trace() << "orcamodel::log_status " << eckit_timer_->elapsed() << " "
      << static_cast<double>(eckit::system::ResourceUsage().maxResidentSetSize()) / 1.0e+9
      << " Gb" << std::endl;
}

}  // namespace orcamodel
