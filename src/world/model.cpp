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
	THROW_ERROR("Not implemented yet");
}


const std::vector<std::shared_ptr<cg::resource<cg::vertex>>>&
cg::world::model::get_vertex_buffers() const
{
	THROW_ERROR("Not implemented yet");
}


const std::vector<std::shared_ptr<cg::resource<unsigned int>>>&
cg::world::model::get_index_buffers() const
{
	THROW_ERROR("Not implemented yet");
}

std::vector<std::filesystem::path>
cg::world::model::get_per_shape_texture_files() const
{
	THROW_ERROR("Not implemented yet");
}


const float4x4 cg::world::model::get_world_matrix() const
{
	THROW_ERROR("Not implemented yet");
}
