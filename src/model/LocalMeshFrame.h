#pragma once
#include <span>
#include <glm/gtc/matrix_transform.hpp>
#include "model/GeoModel.h"
namespace tacz::model {
// Binary-grid translation for geometry far from its bone pivot, only on axes
// where it strictly reduces worst-case half rounding. No geometry/depth bias.
glm::vec3 localMeshOrigin(std::span<Vertex const> vertices);
inline glm::mat4 localMeshMatrix(glm::mat4 const& bone,glm::vec3 origin) {
    return origin==glm::vec3(0) ? bone : glm::translate(bone,origin);
}
}
