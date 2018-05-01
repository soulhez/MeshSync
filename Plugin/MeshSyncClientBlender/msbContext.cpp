#include "pch.h"
#include "MeshSyncClientBlender.h"
#include "msbContext.h"
#include "msbUtils.h"
using namespace mu;


msbContext::msbContext()
{
    m_settings.scene_settings.handedness = ms::Handedness::LeftZUp;
}

msbContext::~msbContext()
{
    if (m_send_future.valid()) {
        m_send_future.wait();
    }
}

void msbContext::setup()
{
    bl::setup();
}

msbSettings& msbContext::getSettings() { return m_settings; }
const msbSettings& msbContext::getSettings() const { return m_settings; }
ms::ScenePtr msbContext::getCurrentScene() const { return m_scene; }

template<class T>
std::shared_ptr<T> msbContext::getCacheOrCreate(std::vector<std::shared_ptr<T>>& cache)
{
    std::shared_ptr<T> ret;
    {
        if (cache.empty()) {
            ret.reset(new T());
        }
        else {
            ret = cache.back();
            cache.pop_back();
        }
    }
    ret->clear();
    return ret;
}


void msbContext::syncAll()
{
    if (!prepare()) return;

    for (auto *o : bl::BData::get().objects()) {
        addObject(o);
    }
    send();
}

void msbContext::syncUpdated()
{
    if (!prepare()) return;
}

void msbContext::addObject(Object * obj)
{
    // todo
    bl::BObject bo(obj);
    bl::BID data_id(bo.data());
    if (data_id.ptr()) {
        auto tc = data_id.typecode();
        if (tc == ID_ME && m_settings.sync_meshes) {
            auto dst = addMesh(get_path(obj));
            dst->refine_settings.flags.swap_faces = true;
            extractMeshData_(dst, bo);
        }
        else if (tc == ID_CA && m_settings.sync_cameras) {
        }
        else if (tc == ID_LA && m_settings.sync_lights) {
        }
    }

}

ms::TransformPtr msbContext::addTransform(const std::string& path)
{
    auto ret = getCacheOrCreate(m_transform_cache);
    ret->path = path;
    m_scene->transforms.push_back(ret);
    return ret;
}

ms::CameraPtr msbContext::addCamera(const std::string& path)
{
    auto ret = getCacheOrCreate(m_camera_cache);
    ret->path = path;
    m_scene->cameras.push_back(ret);
    return ret;
}

ms::LightPtr msbContext::addLight(const std::string& path)
{
    auto ret = getCacheOrCreate(m_light_cache);
    ret->path = path;
    m_scene->lights.push_back(ret);
    return ret;
}

ms::MeshPtr msbContext::addMesh(const std::string& path)
{
    auto ret = getCacheOrCreate(m_mesh_cache);
    ret->path = path;
    m_scene->meshes.push_back(ret);
    return ret;
}

void msbContext::addDeleted(const std::string& path)
{
    m_deleted.push_back(path);
}


// src_: bpy.Material
ms::MaterialPtr msbContext::addMaterial(py::object src)
{
    auto rna = (BPy_StructRNA*)src.ptr();
    auto mat = (Material*)rna->ptr.id.data;

    auto ret = ms::MaterialPtr(new ms::Material());
    ret->name = mat->id.name + 2;
    ret->id = (int)m_scene->materials.size();
    {
        bl::BMaterial bm(mat);
        auto color_src = mat;
        if (bm.use_nodes()) {
            bl::BMaterial node(bm.active_node_material());
            if (node.ptr()) {
                color_src = node.ptr();
            }
        }
        ret->color = float4{ color_src->r, color_src->g, color_src->b, 1.0f };
    }
    m_scene->materials.emplace_back(ret);
    return ret;
}

int msbContext::getMaterialIndex(const Material *mat)
{
    if (mat == nullptr)
        return 0;

    int i = 0;
    for (auto& m : m_scene->materials) {
        if (m->name == mat->id.name + 2)
            return i;
        ++i;
    }
    return 0;
}

// src: bpy.Object
void msbContext::extractTransformData(ms::TransformPtr dst, py::object src)
{
    extractTransformData_(dst, bl::BObject(src));
}
void msbContext::extractTransformData_(ms::TransformPtr dst, bl::BObject src)
{
    extract_local_TRS(src.ptr(), dst->position, dst->rotation, dst->scale);
}


// src: bpy.Object
void msbContext::extractCameraData(ms::CameraPtr dst, py::object src)
{
    extractCameraData_(dst, bl::BObject(src));
}
void msbContext::extractCameraData_(ms::CameraPtr dst, bl::BObject src)
{
    extractTransformData_(dst, src);

    // todo
}


// src: bpy.Object

void msbContext::extractLightData(ms::LightPtr dst, py::object src)
{
    extractLightData_(dst, bl::BObject(src));
}
void msbContext::extractLightData_(ms::LightPtr dst, bl::BObject src)
{
    extractTransformData_(dst, src);
    // todo
}

// src: bpy.Object
void msbContext::extractMeshData(ms::MeshPtr dst, py::object src)
{
    extractMeshData_(dst, bl::BObject(src));
}

void msbContext::extractMeshData_(ms::MeshPtr dst, bl::BObject src)
{
    auto obj = src.ptr();

    // ignore particles
    if (find_modofier(obj, eModifierType_ParticleSystem) || find_modofier(obj, eModifierType_ParticleInstance))
        return;
    // ignore if already added
    if (m_added.find(obj) != m_added.end())
        return;
    m_added.insert(obj);

    // check if mesh is dirty
    {
        bl::BObject bobj(obj);
        bl::BMesh bmesh(bobj.data());
        if (bmesh.ptr()->edit_btmesh) {
            auto bm = bmesh.ptr()->edit_btmesh->bm;
            if (bm->elem_table_dirty) {
                // mesh is editing and dirty. just add to pending list
                dst->clear();
                m_pending.insert(obj);
                return;
            }
        }
    }


    extractTransformData_(dst, src);
    auto task = [this, dst, obj]() {
        doExtractMeshData(*dst, obj);
    };
#ifdef msDebug
    // force single-threaded
    task();
#else
    // add async task
    m_extract_tasks.push_back(task);
#endif
}

void msbContext::exportArmature(bl::BObject src)
{
    std::unique_lock<std::mutex> lock(m_extract_mutex);

    Object *arm_obj = src.ptr();
    if (m_added.find(arm_obj) != m_added.end())
        return;
    m_added.insert(arm_obj);

    auto poses = bl::range((bPoseChannel*)arm_obj->pose->chanbase.first);

    for (auto pose : poses)
    {
        auto bone = pose->bone;
        auto& dst = m_bones[bone];
        dst = addTransform(get_path(arm_obj) + get_path(bone));
        if (m_settings.sync_poses)
            extract_local_TRS(pose, dst->position, dst->rotation, dst->scale);
        else
            extract_local_TRS(bone, dst->position, dst->rotation, dst->scale);
    }
}

void msbContext::doExtractMeshData(ms::Mesh& dst, Object *obj)
{
    bl::BObject bobj(obj);
    bl::BMesh bmesh(bobj.data());

    if (bmesh.ptr()->edit_btmesh) {
        doExtractEditMeshData(dst, obj);
    }
    else {
        doExtractNonEditMeshData(dst, obj);
    }

    // mirror
    if(auto *mirror = (const MirrorModifierData*)find_modofier(obj, eModifierType_Mirror)) {
        if (mirror->flag & MOD_MIR_AXIS_X) dst.refine_settings.flags.mirror_x = 1;
        if (mirror->flag & MOD_MIR_AXIS_Y) dst.refine_settings.flags.mirror_z = 1;
        if (mirror->flag & MOD_MIR_AXIS_Z) dst.refine_settings.flags.mirror_y = 1;
    }

    dst.convertHandedness_Mesh(false, true);
    dst.convertHandedness_BlendShapes(false, true);
}

void msbContext::doExtractNonEditMeshData(ms::Mesh & dst, Object * obj)
{
    bl::BObject bobj(obj);
    bl::BMesh bmesh(bobj.data());
    auto& mesh = *(Mesh*)obj->data;

    auto indices = bmesh.indices();
    auto polygons = bmesh.polygons();
    auto vertices = bmesh.vertices();

    size_t num_indices = indices.size();
    size_t num_polygons = polygons.size();
    size_t num_vertices = vertices.size();

    std::vector<int> material_ids(mesh.totcol);
    for (int mi = 0; mi < mesh.totcol; ++mi)
        material_ids[mi] = getMaterialIndex(mesh.mat[mi]);
    if (material_ids.empty())
        material_ids.push_back(0);

    // vertices
    dst.points.resize_discard(num_vertices);
    for (size_t vi = 0; vi < num_vertices; ++vi) {
        dst.points[vi] = (float3&)vertices[vi].co;
    }

    // faces
    dst.indices.reserve(num_indices);
    dst.counts.resize_discard(num_polygons);
    dst.material_ids.resize_discard(num_polygons);
    {
        int ii = 0;
        for (size_t pi = 0; pi < num_polygons; ++pi) {
            auto& polygon = polygons[pi];
            int material_index = polygon.mat_nr;
            int count = polygon.totloop;
            dst.counts[pi] = count;
            dst.material_ids[pi] = material_ids[material_index];
            dst.indices.resize(dst.indices.size() + count);

            auto *idx = &indices[polygon.loopstart];
            for (int li = 0; li < count; ++li) {
                dst.indices[ii++] = idx[li].v;
            }
        }
    }

    // normals
    if (m_settings.sync_normals == msbNormalSyncMode::PerVertex) {
        // per-vertex
        dst.normals.resize_discard(num_vertices);
        for (size_t vi = 0; vi < num_vertices; ++vi) {
            dst.normals[vi] = to_float3(vertices[vi].no);
        }
    }
    else if (m_settings.sync_normals == msbNormalSyncMode::PerIndex) {
        // per-index
        if (m_settings.calc_per_index_normals)
            bmesh.calc_normals_split();

        auto normals = bmesh.normals();
        if (!normals.empty()) {
            dst.normals.resize_discard(num_indices);
            for (size_t ii = 0; ii < num_indices; ++ii)
                dst.normals[ii] = normals[ii];
        }
    }


    // uv
    if (m_settings.sync_uvs) {
        auto uv = bmesh.uv();
        if (!uv.empty()) {
            dst.uv0.resize_discard(num_indices);
            for (size_t ii = 0; ii < num_indices; ++ii)
                dst.uv0[ii] = uv[ii];
        }
    }

    // colors
    if (m_settings.sync_colors) {
        auto colors = bmesh.colors();
        if (!colors.empty()) {
            dst.colors.resize_discard(num_indices);
            for (size_t ii = 0; ii < num_indices; ++ii)
                dst.colors[ii] = to_float4(colors[ii]);
        }
    }

    // bones
    if (m_settings.sync_bones) {
        auto *arm_mod = (const ArmatureModifierData*)find_modofier(obj, eModifierType_Armature);
        if (arm_mod) {
            // request bake TRS
            dst.refine_settings.flags.apply_local2world = 1;
            dst.refine_settings.local2world = ms::transform(dst.position, invert(dst.rotation), dst.scale);

            auto *arm_obj = arm_mod->object;
            exportArmature(arm_obj);

            int group_index = 0;
            each_vertex_groups(obj, [&](const bDeformGroup *g) {
                auto bone = find_bone(arm_obj, g->name);
                if (bone) {
                    auto trans = findBone(arm_obj, bone);
                    auto b = dst.addBone(trans->path);
                    b->bindpose = extract_bindpose(bone);
                    b->weights.resize_zeroclear(num_vertices);

                    for (int vi = 0; vi < num_vertices; ++vi) {
                        int num_weights = mesh.dvert[vi].totweight;
                        auto& dvert = mesh.dvert[vi];
                        for (int wi = 0; wi < num_weights; ++wi) {
                            if (dvert.dw[wi].def_nr == group_index) {
                                b->weights[vi] = dvert.dw[wi].weight;
                            }
                        }
                    }
                }
                else {
                    mscTrace("bone not found %s\n", bone->name);
                }
                ++group_index;
            });
        }
    }

    // blend shapes
    if (m_settings.sync_blendshapes && mesh.key) {
        RawVector<float3> basis;
        int bi = 0;
        each_keys(&mesh, [&](const KeyBlock *kb) {
            if (bi == 0) { // Basis
                basis.resize_discard(kb->totelem);
                memcpy(basis.data(), kb->data, basis.size() * sizeof(float3));
            }
            else {
                auto bsd = dst.addBlendShape(kb->name);
                bsd->weight = kb->curval * 100.0f;
                bsd->frames.resize(1);
                auto& frame = bsd->frames.back();
                frame.weight = 100.0f;
                frame.points.resize_discard(kb->totelem);
                memcpy(frame.points.data(), kb->data, basis.size() * sizeof(float3));

                size_t len = frame.points.size();
                for (size_t i = 0; i < len; ++i) {
                    frame.points[i] -= basis[i];
                }
            }
            ++bi;
        });
    }


#if 0
    // lines
    // (blender doesn't include lines & points in polygons - MPoly::totloop is always >= 3)
    {
        auto edges = bmesh.edges();

        std::vector<bool> point_shared(num_vertices);
        for (size_t pi = 0; pi < num_polygons; ++pi) {
            auto& polygon = polygons[pi];
            int count = polygon.totloop;
            auto *idx = &indices[polygon.loopstart];
            for (int li = 0; li < count; ++li) {
                point_shared[idx[li].v] = true;
            }
        }

        size_t lines_begin = dst.indices.size();
        size_t num_lines = 0;
        for (auto edge : edges) {
            if (!point_shared[edge.v1] || !point_shared[edge.v2]) {
                ++num_lines;
                dst.counts.push_back(2);
                dst.indices.push_back(edge.v1);
                dst.indices.push_back(edge.v2);
            }
        }

        if (num_lines > 0) {
            num_indices = dst.indices.size();

            if (!dst.normals.empty() && m_settings.sync_normals == msbNormalSyncMode::PerIndex) {
                dst.normals.resize(num_indices, float3::zero());
            }
            if (!dst.uv0.empty()) {
                dst.uv0.resize(num_indices, float2::zero());
            }
            if (!dst.colors.empty()) {
                auto colors = bmesh.colors();
                dst.colors.resize(num_indices, float4::one());
                for (size_t ii = lines_begin; ii < num_indices; ++ii) {
                    int vi = dst.indices[ii];
                    dst.colors[ii] = to_float4(colors[vi]);
                }
            }
        }
    }
#endif
}

void msbContext::doExtractEditMeshData(ms::Mesh & dst, Object * obj)
{
    bl::BObject bobj(obj);
    bl::BMesh bmesh(bobj.data());
    bl::BEditMesh emesh(bmesh.ptr()->edit_btmesh);
    auto& mesh = *(Mesh*)obj->data;

    auto polygons = emesh.polygons();
    auto triangles = emesh.triangles();
    auto vertices = emesh.vertices();

    size_t num_triangles = triangles.size();
    size_t num_vertices = vertices.size();
    size_t num_indices = triangles.size() * 3;

    std::vector<int> material_ids(mesh.totcol);
    for (int mi = 0; mi < mesh.totcol; ++mi)
        material_ids[mi] = getMaterialIndex(mesh.mat[mi]);
    if (material_ids.empty())
        material_ids.push_back(0);

    // vertices
    dst.points.resize_discard(num_vertices);
    for (size_t vi = 0; vi < num_vertices; ++vi) {
        dst.points[vi] = (float3&)vertices[vi]->co;
    }

    // faces
    {
        dst.indices.resize(num_indices);
        dst.counts.resize_discard(num_triangles);
        dst.material_ids.resize_discard(num_triangles);

        size_t ii = 0;
        for (size_t ti = 0; ti < num_triangles; ++ti) {
            auto& triangle = triangles[ti];

            int material_index = 0;
            int polygon_index = triangle[0]->f->head.index;
            if (polygon_index < polygons.size())
                material_index = material_ids[polygons[polygon_index]->mat_nr];
            dst.material_ids[ti] = material_index;

            dst.counts[ti] = 3;
            for (auto *idx : triangle)
                dst.indices[ii++] = idx->v->head.index;
        }
    }

    // normals
    if (m_settings.sync_normals == msbNormalSyncMode::PerVertex) {
        // per-vertex
        dst.normals.resize_discard(num_vertices);
        for (size_t vi = 0; vi < num_vertices; ++vi)
            dst.normals[vi] = to_float3(vertices[vi]->no);
    }
    else if (m_settings.sync_normals == msbNormalSyncMode::PerIndex) {
        // per-index
        dst.normals.resize_discard(num_indices);
        size_t ii = 0;
        for (size_t ti = 0; ti < num_triangles; ++ti) {
            auto& triangle = triangles[ti];
            for (auto *idx : triangle)
                dst.normals[ii++] = -bl::BM_loop_calc_face_normal(*idx);
        }
    }

    // uv
    if (m_settings.sync_uvs) {
        int offset = emesh.uv_data_offset();
        if (offset != -1) {
            dst.uv0.resize_discard(num_indices);
            size_t ii = 0;
            for (size_t ti = 0; ti < num_triangles; ++ti) {
                auto& triangle = triangles[ti];
                for (auto *idx : triangle)
                    dst.uv0[ii++] = *(float2*)((char*)idx->head.data + offset);
            }
        }
    }
}


ms::TransformPtr msbContext::findBone(const Object *armature, const Bone *bone)
{
    std::unique_lock<std::mutex> lock(m_extract_mutex);

    auto it = m_bones.find(bone);
    return it != m_bones.end() ? it->second : nullptr;
}


bool msbContext::isSending() const
{
    return m_send_future.valid() && m_send_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout;
}

void msbContext::flushPendingList()
{
    if (!m_pending.empty() && prepare()) {
        std::swap(m_pending, m_pending_tmp);
        for (auto p : m_pending_tmp)
            addObject(p);
        m_pending_tmp.clear();

        send();
    }
}

bool msbContext::prepare()
{
    if (isSending()) return false;

    return true;
}

void msbContext::send()
{
    if (isSending())
    {
        // previous request is not completed yet. skip
        m_extract_tasks.clear();
        return;
    }

    // get vertex data in parallel
    parallel_for_each(m_extract_tasks.begin(), m_extract_tasks.end(), [](task_t& task) {
        task();
    });
    m_extract_tasks.clear();

    // todo
    //for (auto& v : m_scene.cameras)
    //    v->transform.rotation = rotateX(-90.0f * Deg2Rad) * swap_yz(v->transform.rotation);
    //for (auto& v : m_scene.lights)
    //    v->transform.rotation = rotateX(-90.0f * Deg2Rad) * swap_yz(v->transform.rotation);


    // return objects to cache
    for (auto& v : m_message.scene.transforms) { m_transform_cache.push_back(v); }
    for (auto& v : m_message.scene.cameras) { m_camera_cache.push_back(v); }
    for (auto& v : m_message.scene.lights) { m_light_cache.push_back(v); }
    for (auto& v : m_message.scene.meshes) { m_mesh_cache.push_back(v); }

    // setup send data
    std::swap(m_scene->meshes, m_mesh_send);
    m_message.scene = *m_scene;
    m_scene->clear();
    m_bones.clear();
    m_added.clear();

    // kick async send
    m_send_future = std::async(std::launch::async, [this]() {
        ms::Client client(m_settings.client_settings);

        // notify scene begin
        {
            ms::FenceMessage fence;
            fence.type = ms::FenceMessage::FenceType::SceneBegin;
            client.send(fence);
        }

        // send transform, camera, etc

        auto scene_settings = m_settings.scene_settings;
        float scale_factor = 1.0f / m_settings.scene_settings.scale_factor;
        scene_settings.scale_factor = 1.0f;
        scene_settings.handedness = ms::Handedness::Left;
        bool swap_x = false, swap_yz = true;

        m_message.scene.settings = scene_settings;

        auto& scene = m_message.scene;
        if (scale_factor != 1.0f) {
            for (auto& obj : scene.transforms) { obj->applyScaleFactor(scale_factor); }
            for (auto& obj : scene.cameras) { obj->applyScaleFactor(scale_factor); }
            for (auto& obj : scene.lights) { obj->applyScaleFactor(scale_factor); }
        }
        client.send(m_message);

        // send deleted
        if (!m_deleted.empty()) {
            ms::DeleteMessage del;
            for (auto& path : m_deleted) {
                del.targets.push_back({path, 0});
            }
            client.send(del);
        }
        m_deleted.clear();

        // convert and send meshes
        parallel_for_each(m_mesh_send.begin(), m_mesh_send.end(), [&](ms::MeshPtr& pmesh) {
            auto& mesh = *pmesh;
            mesh.setupFlags();
            mesh.flags.apply_trs = true;
            mesh.flags.has_refine_settings = true;
            if (!mesh.flags.has_normals)
                mesh.refine_settings.flags.gen_normals = true;
            if (!mesh.flags.has_tangents)
                mesh.refine_settings.flags.gen_tangents = true;
            if (scale_factor != 1.0f)
                mesh.applyScaleFactor(scale_factor);
        });
        for(auto& pmesh : m_mesh_send) {
            ms::SetMessage set;
            set.scene.settings = scene_settings;
            set.scene.meshes = { pmesh };
            client.send(set);
        }
        m_mesh_send.clear();

        // notify scene end
        {
            ms::FenceMessage fence;
            fence.type = ms::FenceMessage::FenceType::SceneEnd;
            client.send(fence);
        }
    });
}
