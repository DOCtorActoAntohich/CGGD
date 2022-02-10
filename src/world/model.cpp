#define TINYOBJLOADER_IMPLEMENTATION

#include "model.h"

#include "utils/error_handler.h"

#include <linalg.h>


using namespace linalg::aliases;
using namespace cg::world;

cg::world::model::model() {}

cg::world::model::~model() {}


void cg::world::model::load_obj(const std::filesystem::path& model_path)
{
    using tuple_int3 = std::tuple<int32_t, int32_t, int32_t>;

    tinyobj::ObjReaderConfig reader_config;
    reader_config.mtl_search_path = model_path.parent_path();
    reader_config.triangulate = true;

    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(model_path, reader_config) && !reader.Error().empty()) {
        THROW_ERROR(reader.Error());
    }

    auto& attrib = reader.GetAttrib();
    auto& shapes = reader.GetShapes();
    auto& materials = reader.GetMaterials();

    size_t shape_id = 0;
    textures.resize(shapes.size());

    for (const auto& shape : shapes) {
        const auto& mesh = shape.mesh;

        size_t index_offset = 0;

        uint32_t vertex_buffer_size = 0;
        uint32_t index_buffer_size  = 0;

        std::map<tuple_int3, uint32_t> index_map;

        for (size_t f = 0; f < mesh.num_face_vertices.size(); ++f) {
            for (size_t v = 0; v < mesh.num_face_vertices[f]; ++v) {
                tinyobj::index_t idx = mesh.indices[index_offset + v];
                auto idx_tuple = std::make_tuple(
                    idx.vertex_index,
                    idx.normal_index,
                    idx.texcoord_index
                );
                if (index_map.count(idx_tuple) == 0) {
                    index_map[idx_tuple] = vertex_buffer_size;
                    ++vertex_buffer_size;
                }
                ++index_buffer_size;
            }
            index_offset += mesh.num_face_vertices[f];
        }
        vertex_buffers.push_back(
            std::make_shared<cg::resource<cg::vertex>>(vertex_buffer_size)
        );
        index_buffers.push_back(
            std::make_shared<cg::resource<uint32_t>>(index_buffer_size)
        );
        if (!materials[mesh.material_ids[0]].diffuse_texname.empty()) {
            textures[shape_id] = model_path.parent_path() /
                materials[mesh.material_ids[0]].diffuse_texname;
        }
        ++shape_id;
    }

    shape_id = 0;
    for (const auto& shape : shapes) {
        const auto& mesh = shape.mesh;

        size_t index_offset = 0;

        uint32_t vertex_buffer_id = 0;
        uint32_t index_buffer_id  = 0;

        auto vertex_buffer = vertex_buffers[shape_id];
        auto index_buffer  = index_buffers[shape_id];

        std::map<tuple_int3, uint32_t> index_map;

        for (size_t f = 0; f < mesh.num_face_vertices.size(); ++f) {
            int32_t fv = mesh.num_face_vertices[f];

            float3 normal;
            if (mesh.indices[index_offset].normal_index < 0) {
                static auto _get_vertices = [&](tinyobj::index_t id) {
                    return float3{
                        attrib.vertices[3 * id.vertex_index + 0],
                        attrib.vertices[3 * id.vertex_index + 1],
                        attrib.vertices[3 * id.vertex_index + 2]
                    };
                };

                auto a_id = mesh.indices[index_offset + 0];
                auto b_id = mesh.indices[index_offset + 1];
                auto c_id = mesh.indices[index_offset + 2];

                auto a = _get_vertices(a_id);
                auto b = _get_vertices(b_id);
                auto c = _get_vertices(c_id);

                normal = normalize(cross(b - a, c - a));
            }

            for (size_t v = 0; v < mesh.num_face_vertices[f]; ++v) {
                auto idx = mesh.indices[index_offset + v];
                auto idx_tuple = std::make_tuple(
                    idx.vertex_index,
                    idx.normal_index,
                    idx.texcoord_index
                );

                if (index_map.count(idx_tuple) == 0) {
                    auto& vertex = vertex_buffer->item(vertex_buffer_id);

                    vertex.x = attrib.vertices[3 * idx.vertex_index + 0];
                    vertex.y = attrib.vertices[3 * idx.vertex_index + 1];
                    vertex.z = attrib.vertices[3 * idx.vertex_index + 2];

                    if (idx.normal_index > -1) {
                        vertex.nx = attrib.normals[3 * idx.normal_index + 0];
                        vertex.ny = attrib.normals[3 * idx.normal_index + 1];
                        vertex.nz = attrib.normals[3 * idx.normal_index + 2];
                    }
                    else {
                        vertex.nx = normal.x;
                        vertex.ny = normal.y;
                        vertex.nz = normal.z;
                    }

                    if (idx.texcoord_index > -1) {
                        vertex.u = attrib.texcoords[2 * idx.texcoord_index + 0];
                        vertex.v = attrib.texcoords[2 * idx.texcoord_index + 1];
                    }
                    else {
                        vertex.u = 0.5f;
                        vertex.v = 0.5f;
                    }

                    if (materials.size() > 0) {
                        const auto& material = materials[mesh.material_ids[f]];

                        vertex.ambient_r = material.ambient[0];
                        vertex.ambient_g = material.ambient[1];
                        vertex.ambient_b = material.ambient[2];

                        vertex.diffuse_r = material.diffuse[0];
                        vertex.diffuse_g = material.diffuse[1];
                        vertex.diffuse_b = material.diffuse[2];

                        vertex.emissive_r = material.emission[0];
                        vertex.emissive_g = material.emission[1];
                        vertex.emissive_b = material.emission[2];
                    }
                    index_map[idx_tuple] = vertex_buffer_id;
                    ++vertex_buffer_id;
                }
                index_buffer->item(index_buffer_id) = index_map[idx_tuple];
                ++index_buffer_id;
            }
            index_offset += fv;
        }
        ++shape_id;
    }
}

const std::vector<std::shared_ptr<cg::resource<cg::vertex>>>&
cg::world::model::get_vertex_buffers() const
{
    return vertex_buffers;
}


const std::vector<std::shared_ptr<cg::resource<unsigned int>>>&
cg::world::model::get_index_buffers() const
{
    return index_buffers;
}

std::vector<std::filesystem::path>
cg::world::model::get_per_shape_texture_files() const
{
    return textures;
}


const float4x4 cg::world::model::get_world_matrix() const
{
    return float4x4{
        { 1, 0, 0, 0 },
        { 0, 1, 0, 0 },
        { 0, 0, 1, 0 },
        { 0, 0, 0, 1 }
    };
}
