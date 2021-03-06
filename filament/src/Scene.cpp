/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "details/Scene.h"

#include "components/LightManager.h"
#include "components/RenderableManager.h"

#include "details/Culler.h"
#include "details/Engine.h"
#include "details/IndirectLight.h"
#include "details/GpuLightBuffer.h"
#include "details/Skybox.h"

#include <utils/compiler.h>
#include <utils/EntityManager.h>
#include <utils/Range.h>
#include <utils/Zip2Iterator.h>

#include <algorithm>

using namespace math;
using namespace utils;

namespace filament {
namespace details {

// ------------------------------------------------------------------------------------------------

FScene::FScene(FEngine& engine) :
        mEngine(engine),
        mIndirectLight(engine.getDefaultIndirectLight()),
        mGpuLightData(engine) {
}

FScene::~FScene() noexcept = default;


void FScene::prepare(const math::mat4f& worldOriginTansform) {
    // TODO: can we skip this in most cases? Since we rely on indices staying the same,
    //       we could only skip, if nothing changed in the RCM.

    FEngine& engine = mEngine;
    EntityManager& em = engine.getEntityManager();
    FRenderableManager& rcm = engine.getRenderableManager();
    FTransformManager& tcm = engine.getTransformManager();
    FLightManager& lcm = engine.getLightManager();
    // go through the list of entities, and gather the data of those that are renderables
    auto& sceneData = mRenderableData;
    auto& lightData = mLightData;
    auto const& entities = mEntities;


    // NOTE: we can't know in advance how many entities are renderable or lights because the corresponding
    // component can be added after the entity is added to the scene.

    // for the purpose of allocation, we'll assume all our entities are renderables
    size_t capacity = entities.size();
    // we need the capacity to be multiple of 16 for SIMD loops
    capacity = (capacity + 0xF) & ~0xF;
    // we need 1 extra entry at the end for teh summed primitive count
    capacity = capacity + 1;

    sceneData.clear();
    if (sceneData.capacity() < capacity) {
        sceneData.setCapacity(capacity);
    }

    lightData.clear();
    if (lightData.capacity() < capacity) {
        lightData.setCapacity(capacity);
    }
    // the first entries are reserved for the directional lights (currently only one)
    lightData.resize(DIRECTIONAL_LIGHTS_COUNT);


    // find the max intensity directional light index in our local array
    float maxIntensity = 0;

    for (Entity e : entities) {
        if (!em.isAlive(e))
            continue;

        // getInstance() always returns null if the entity is the Null entity
        // so we don't need to check for that, but we need to check it's alive
        auto ri = rcm.getInstance(e);
        auto li = lcm.getInstance(e);
        if (!ri & !li)
            continue;

        // get the world transform
        auto ti = tcm.getInstance(e);
        const mat4f worldTransform = worldOriginTansform * tcm.getWorldTransform(ti);

        // don't even draw this object if it doesn't have a transform (which shouldn't happen
        // because one is always created when creating a Renderable component).
        if (ri && ti) {
            // compute the world AABB so we can perform culling
            const Box worldAABB = rigidTransform(rcm.getAABB(ri), worldTransform);

            // we know there is enough space in the array
            sceneData.push_back_unsafe(
                    ri,
                    worldTransform,
                    rcm.getVisibility(ri),
                    rcm.getUbh(ri),
                    rcm.getBonesUbh(ri),
                    worldAABB.center,
                    0,
                    rcm.getLayerMask(ri),
                    worldAABB.halfExtent,
                    {}, {});
        }

        if (li) {
            // find the dominant directional light
            if (UTILS_UNLIKELY(lcm.isDirectionalLight(li))) {
                // we don't store the directional lights, because we only have a single one
                if (lcm.getIntensity(li) >= maxIntensity) {
                    float3 d = lcm.getLocalDirection(li);
                    // using the inverse-transpose handles non-uniform scaling
                    d = normalize(transpose(inverse(worldTransform.upperLeft())) * d);
                    // TODO: allow lightData.front() = { ... } syntax
                    lightData.elementAt<FScene::POSITION_RADIUS>(0) = {};
                    lightData.elementAt<FScene::DIRECTION>(0)       = d;
                    lightData.elementAt<FScene::LIGHT_INSTANCE>(0)  = li;
                    lightData.elementAt<FScene::VISIBILITY>(0)      = {};
                }
            } else {
                const float4 p = worldTransform * float4{ lcm.getLocalPosition(li), 1 };
                float3 d = 0;
                if (!lcm.isPointLight(li) || lcm.isIESLight(li)) {
                    d = lcm.getLocalDirection(li);
                    // using the inverse-transpose handles non-uniform scaling
                    d = normalize(transpose(inverse(worldTransform.upperLeft())) * d);
                }
                lightData.push_back_unsafe(
                        float4{ p.xyz, lcm.getRadius(li) }, d, li, {});
            }
        }
    }
}

void FScene::updateUBOs(utils::Range<uint32_t> visibleRenderables) const noexcept {
    FRenderableManager& rcm = mEngine.getRenderableManager();
    auto& sceneData = mRenderableData;
    for (uint32_t i : visibleRenderables) {
        auto ri = sceneData.elementAt<RENDERABLE_INSTANCE>(i);
        rcm.updateLocalUBO(ri, sceneData.elementAt<WORLD_TRANSFORM>(i));
    }
}

void FScene::terminate(FEngine& engine) {
    // free-up the lights buffer
    mGpuLightData.terminate(engine);
}

void FScene::prepareLights(const CameraInfo& camera) noexcept {
    FLightManager& lcm = mEngine.getLightManager();
    GpuLightBuffer& gpuLightData = mGpuLightData;
    FScene::LightSoa& lightData = getLightData();

    /*
     * Here we copy our lights data into the GPU buffer, some lights might be left out if there
     * are more than the GPU buffer allows (i.e. 255).
     *
     * Sorting light by distance to the camera for dropping the ones in excess doesn't
     * work well because a light far from the camera could light an object close to it
     * (e.g. a search light).
     *
     * When we have too many lights, there is nothing better we can do though.
     * However, when the froxelization "record buffer" runs out of space, it's better to drop
     * froxels far from the camera instead. This would happen during froxelization.
     */

    // don't count the directional light
    if (UTILS_UNLIKELY(lightData.size() > CONFIG_MAX_LIGHT_COUNT + DIRECTIONAL_LIGHTS_COUNT)) {
        // pre-compute the lights' distance to the camera, for sorting below.
        float3 const position = camera.getPosition();
        float distances[CONFIG_MAX_LIGHT_COUNT]; // 1 KiB
        // skip directional light
        for (size_t i = DIRECTIONAL_LIGHTS_COUNT, c = lightData.size(); i < c; ++i) {
            // TODO: this should take spot-light direction into account
            // TODO: maybe we could also take the intensity into account
            float4 s = lightData.elementAt<FScene::POSITION_RADIUS>(i);
            distances[i] = std::max(0.0f, length(position - s.xyz) - s.w);
        }

        // skip directional light
        Zip2Iterator<FScene::LightSoa::iterator, float*> b = { lightData.begin(), distances };
        std::sort(b + DIRECTIONAL_LIGHTS_COUNT, b + lightData.size(),
                [](auto const& lhs, auto const& rhs) { return lhs.second < rhs.second; });

        lightData.resize(std::min(lightData.size(), CONFIG_MAX_LIGHT_COUNT + DIRECTIONAL_LIGHTS_COUNT));
    }

    assert(lightData.size() <= CONFIG_MAX_LIGHT_COUNT + 1);

    auto const* UTILS_RESTRICT positions    = lightData.data<FScene::POSITION_RADIUS>();
    auto const* UTILS_RESTRICT directions   = lightData.data<FScene::DIRECTION>();
    auto const* UTILS_RESTRICT instances    = lightData.data<FScene::LIGHT_INSTANCE>();
    for (size_t i = DIRECTIONAL_LIGHTS_COUNT, c = lightData.size(); i < c; ++i) {
        GpuLightBuffer::LightIndex gpuIndex = GpuLightBuffer::LightIndex(i - DIRECTIONAL_LIGHTS_COUNT);
        GpuLightBuffer::LightParameters& lp = gpuLightData.getLightParameters(gpuIndex);
        auto li = instances[i];
        lp.positionFalloff      = { positions[i].xyz, lcm.getSquaredFalloffInv(li) };
        lp.colorIntensity       = { lcm.getColor(li), lcm.getIntensity(li) };
        lp.directionIES         = { directions[i], 0 };
        lp.spotScaleOffset.xy   = { lcm.getSpotParams(li).scaleOffset };
    }

    gpuLightData.invalidate(0, lightData.size());
    gpuLightData.commit(mEngine);
}

void FScene::addEntity(Entity entity) {
    mEntities.insert(entity);
}

void FScene::remove(Entity entity) {
    mEntities.erase(entity);
}

size_t FScene::getRenderableCount() const noexcept {
    FEngine& engine = mEngine;
    EntityManager& em = engine.getEntityManager();
    FRenderableManager& rcm = engine.getRenderableManager();
    size_t count = 0;
    auto const& entities = mEntities;
    for (Entity e : entities) {
        count += em.isAlive(e) && rcm.getInstance(e) ? 1 : 0;
    }
    return count;
}

size_t FScene::getLightCount() const noexcept {
    FEngine& engine = mEngine;
    EntityManager& em = engine.getEntityManager();
    FLightManager& lcm = engine.getLightManager();
    size_t count = 0;
    auto const& entities = mEntities;
    for (Entity e : entities) {
        count += em.isAlive(e) && lcm.getInstance(e) ? 1 : 0;
    }
    return count;
}

void FScene::setSkybox(FSkybox const* skybox) noexcept {
    std::swap(mSkybox, skybox);
    if (skybox) {
        remove(skybox->getEntity());
    }
    if (mSkybox) {
        addEntity(mSkybox->getEntity());
    }
}

void FScene::computeBounds(
        Aabb& UTILS_RESTRICT castersBox,
        Aabb& UTILS_RESTRICT receiversBox,
        uint32_t visibleLayers) const noexcept {
    using State = FRenderableManager::Visibility;

    // Compute the scene bounding volume
    RenderableSoa const& UTILS_RESTRICT soa = mRenderableData;
    float3 const* const UTILS_RESTRICT worldAABBCenter = soa.data<WORLD_AABB_CENTER>();
    float3 const* const UTILS_RESTRICT worldAABBExtent = soa.data<WORLD_AABB_EXTENT>();
    uint8_t const* const UTILS_RESTRICT layers = soa.data<LAYERS>();
    State const* const UTILS_RESTRICT visibility = soa.data<VISIBILITY_STATE>();
    size_t c = soa.size();
    for (size_t i = 0; i < c; i++) {
        if (layers[i] & visibleLayers) {
            const Aabb aabb{ worldAABBCenter[i] - worldAABBExtent[i],
                             worldAABBCenter[i] + worldAABBExtent[i] };
            if (visibility[i].castShadows) {
                castersBox.min = min(castersBox.min, aabb.min);
                castersBox.max = max(castersBox.max, aabb.max);
            }
            if (visibility[i].receiveShadows) {
                receiversBox.min = min(receiversBox.min, aabb.min);
                receiversBox.max = max(receiversBox.max, aabb.max);
            }
        }
    }
}

} // namespace details

// ------------------------------------------------------------------------------------------------
// Trampoline calling into private implementation
// ------------------------------------------------------------------------------------------------

using namespace details;

void Scene::setSkybox(Skybox const* skybox) noexcept {
    upcast(this)->setSkybox(upcast(skybox));
}

void Scene::setIndirectLight(IndirectLight const* ibl) noexcept {
    upcast(this)->setIndirectLight(upcast(ibl));
}

void Scene::addEntity(Entity entity) {
    upcast(this)->addEntity(entity);
}

void Scene::remove(Entity entity) {
    upcast(this)->remove(entity);
}

size_t Scene::getRenderableCount() const noexcept {
    return upcast(this)->getRenderableCount();
}

size_t Scene::getLightCount() const noexcept {
    return upcast(this)->getLightCount();
}

} // namespace filament
