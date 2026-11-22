import json
import os
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Matrix


def to_blender(v):
    return np.asarray(v, dtype=np.float64)[..., [0, 2, 1]]


def load_mesh(path):
    n, has_uv = np.fromfile(path, dtype=np.uint32, count=2)
    data = np.fromfile(path, dtype=np.float32, offset=8)
    verts = data[:n * 7].reshape(n, 7)
    uv_page = data[n * 7:n * 10].reshape(n, 3) if has_uv else None
    return verts, uv_page


def emission_material(name, image):
    mat = bpy.data.materials.new(name)
    mat.use_backface_culling = True
    nodes = mat.node_tree.nodes
    nodes.clear()
    out = nodes.new('ShaderNodeOutputMaterial')
    emit = nodes.new('ShaderNodeEmission')
    mat.node_tree.links.new(emit.outputs['Emission'], out.inputs['Surface'])
    if image is None:
        emit.inputs['Color'].default_value = (0.05, 0.0, 0.05, 1.0)
        mat.diffuse_color = (0.05, 0.0, 0.05, 1.0)
        return mat
    tex = nodes.new('ShaderNodeTexImage')
    tex.image = image
    tex.interpolation = 'Linear'
    uv = nodes.new('ShaderNodeUVMap')
    uv.uv_map = 'atlas'
    mat.node_tree.links.new(uv.outputs['UV'], tex.inputs['Vector'])
    mat.node_tree.links.new(tex.outputs['Color'], emit.inputs['Color'])
    nodes.active = tex
    return mat


def build_level(scene_json, verts, uv_page, collection):
    name = scene_json['name']
    n = len(verts)
    tris = n // 3
    pos = to_blender(verts[:, :3])
    normal = to_blender(verts[:, 3:6]).reshape(tris, 3, 3)[:, 0]
    p = pos.reshape(tris, 3, 3)
    geometric = np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0])
    order = np.tile(np.array([0, 1, 2]), (tris, 1))
    order[(geometric * normal).sum(1) < 0] = [0, 2, 1]
    index = (np.arange(tris)[:, None] * 3 + order).ravel()
    pos = pos[index]

    mesh = bpy.data.meshes.new(name)
    mesh.vertices.add(n)
    mesh.vertices.foreach_set('co', pos.astype(np.float32).ravel())
    mesh.loops.add(n)
    mesh.loops.foreach_set('vertex_index', np.arange(n, dtype=np.int32))
    mesh.polygons.add(tris)
    mesh.polygons.foreach_set('loop_start', np.arange(0, n, 3, dtype=np.int32))
    mesh.update()
    cell = mesh.attributes.new('cell', 'INT', 'FACE')
    cell.data.foreach_set('value', verts[0::3, 6].astype(np.int32))

    pages = scene_json['pages']
    images = [bpy.data.images.load(path, check_existing=True) for path in pages]
    for i, image in enumerate(images):
        mesh.materials.append(emission_material(f'{name}_page{i}', image))
    mesh.materials.append(emission_material(f'{name}_unseen', None))
    if uv_page is not None:
        uv = uv_page[index]
        page_size = float(scene_json['pageSize'])
        coords = np.stack([uv[:, 0] / page_size, 1.0 - uv[:, 1] / page_size], axis=1)
        layer = mesh.uv_layers.new(name='atlas')
        layer.uv.foreach_set('vector', coords.astype(np.float32).ravel())
        page = uv[0::3, 2].astype(np.int32)
        page[page < 0] = len(images)
    else:
        page = np.full(tris, len(images), dtype=np.int32)
    mesh.polygons.foreach_set('material_index', page)

    bm = bmesh.new()
    bm.from_mesh(mesh)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1.0 / 1024.0)
    bm.to_mesh(mesh)
    bm.free()
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    collection.objects.link(obj)
    return obj


def build_cameras(scene_json, export_dir, collection, scene):
    cameras = []
    for frame, cam in enumerate(scene_json['cameras'], start=1):
        m = np.array(cam['m'], dtype=np.float64).reshape(3, 3)
        right, up, forward = to_blender(m[:, 0]), to_blender(m[:, 1]), to_blender(m[:, 2])
        rotation = np.stack([right, up, -forward], axis=1)
        world = Matrix.Identity(4)
        for r in range(3):
            for c in range(3):
                world[r][c] = rotation[r, c]
            world[r][3] = to_blender(cam['pos'])[r]
        data = bpy.data.cameras.new(cam['name'])
        data.sensor_fit = 'HORIZONTAL'
        data.sensor_width = 36.0
        data.lens = cam['f'] * 36.0 / 640.0
        data.clip_start = 0.1
        data.clip_end = 5000.0
        backdrop = os.path.join(export_dir, 'backdrops', cam['name'] + '.png')
        if os.path.exists(backdrop):
            bg = data.background_images.new()
            bg.image = bpy.data.images.load(backdrop, check_existing=True)
            bg.alpha = 1.0
            data.show_background_images = True
        obj = bpy.data.objects.new(cam['name'], data)
        obj.matrix_world = world
        collection.objects.link(obj)
        marker = scene.timeline_markers.new(cam['name'], frame=frame)
        marker.camera = obj
        cameras.append(obj)
    return cameras


def main():
    args = sys.argv[sys.argv.index('--') + 1:]
    export_dir = os.path.abspath(args[0])
    with open(os.path.join(export_dir, 'scene.json')) as f:
        scene_json = json.load(f)
    name = scene_json['name']
    out = os.path.abspath(args[1]) if len(args) > 1 else os.path.join(export_dir, name + '.blend')
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.name = name
    scene.render.resolution_x = 640
    scene.render.resolution_y = 480
    scene.display.shading.show_backface_culling = True
    level_collection = bpy.data.collections.new('Level')
    camera_collection = bpy.data.collections.new('Cameras')
    scene.collection.children.link(level_collection)
    scene.collection.children.link(camera_collection)
    verts, uv_page = load_mesh(os.path.join(export_dir, 'mesh.bin'))
    build_level(scene_json, verts, uv_page, level_collection)
    cameras = build_cameras(scene_json, export_dir, camera_collection, scene)
    scene.camera = cameras[0]
    scene.frame_start = 1
    scene.frame_end = len(cameras)
    bpy.ops.wm.save_as_mainfile(filepath=out, relative_remap=True)
    print(f'saved {out}: {len(verts) // 3} triangles, {len(cameras)} cameras')


main()
