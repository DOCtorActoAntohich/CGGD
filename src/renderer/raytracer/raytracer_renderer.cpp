#include "raytracer_renderer.h"

#include "utils/resource_utils.h"

#include <iostream>


void cg::renderer::ray_tracing_renderer::init()
{
    model = std::make_shared<cg::world::model>();
    model->load_obj(settings->model_path);

    camera = std::make_shared<cg::world::camera>();
    camera->set_width(static_cast<float>(settings->width));
    camera->set_height(static_cast<float>(settings->height));
    camera->set_position(float3{
        settings->camera_position[0],
        settings->camera_position[1],
        settings->camera_position[2],
    });
    camera->set_theta(settings->camera_theta);
    camera->set_phi(settings->camera_phi);
    camera->set_angle_of_view(settings->camera_angle_of_view);
    camera->set_z_near(settings->camera_z_near);
    camera->set_z_far(settings->camera_z_far);

    render_target = std::make_shared<cg::resource<cg::unsigned_color>>(
        settings->width, settings->height
    );

    raytracer = std::make_shared<cg::renderer::raytracer<cg::vertex, cg::unsigned_color>>();
    raytracer->set_render_target(render_target);
    raytracer->set_viewport(settings->width, settings->height);
    raytracer->set_vertex_buffers(model->get_vertex_buffers());
    raytracer->set_index_buffers(model->get_index_buffers());

    lights.push_back({
        float3{ 0, 1.58f, -0.03f },
        float3{ 0.78f, 0.78f, 0.78f },
    });

    shadow_raytracer = std::make_shared<cg::renderer::raytracer<cg::vertex, cg::unsigned_color>>();
}

void cg::renderer::ray_tracing_renderer::destroy() {}

void cg::renderer::ray_tracing_renderer::update() {}

void cg::renderer::ray_tracing_renderer::render()
{
    raytracer->clear_render_target({ 0, 0, 0 });
    raytracer->miss_shader = [](const ray& ray) {
        payload payload{};
        payload.color = { 0.0f, 0.0f, (ray.direction.y + 1.0f) * 0.5f };
        return payload;
    };

    std::random_device random_device;
    std::mt19937 rng(random_device());
    std::uniform_real_distribution<float> uniform_distribution(-1.0f, 1.0f);


    raytracer->closest_hit_shader = [&](const ray& ray,
            payload& payload,
            const triangle<cg::vertex>& triangle,
            size_t depth)
    {
        float3 position = ray.position + ray.direction * payload.t;
        float3 normal = normalize(
            payload.bary.x * triangle.na +
            payload.bary.y * triangle.nb +
            payload.bary.z * triangle.nc
        );
        float3 result_color = { 0.0f, 0.0f, 0.0f };

        for (auto& light : lights) {
            cg::renderer::ray to_light(position, light.position - position);

            auto shadow_payload = shadow_raytracer->trace_ray(
                to_light, 1, length(light.position - position)
            );

            if (shadow_payload.t >= 0.0f) {
                continue;
            }
            result_color +=
                triangle.diffuse *
                (light.color / 2) *
                std::max(
                    dot(normal, to_light.direction), 0.0f
                );
        }


        payload.color = cg::color::from_float3(result_color);
        return payload;
    };
    raytracer->build_acceleration_structure();


    shadow_raytracer->miss_shader = [](const ray& ray) {
        payload payload{};
        payload.t = -1.0f;
        return payload;
    };
    shadow_raytracer->any_hit_shader = [](
            const ray& ray,
            payload& payload,
            const triangle<cg::vertex>& triangle)
    {
        return payload;
    };
    shadow_raytracer->acceleration_structures = raytracer->acceleration_structures;



    auto start = std::chrono::high_resolution_clock::now();

    raytracer->ray_generation(
        camera->get_position(), camera->get_direction(),
        camera->get_right(), camera->get_up(),
        settings->raytracing_depth,
        settings->accumulation_num
    );

    auto end = std::chrono::high_resolution_clock::now();

    std::chrono::duration<float, std::milli> raytracing_duration = end - start;
    std::cout << "Raytracing took " << raytracing_duration.count() << " ms" << std::endl;

    cg::utils::save_resource(*render_target, settings->result_path);
}