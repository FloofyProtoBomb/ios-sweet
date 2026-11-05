#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parent))
from compile_a619_offline import layout_contract, render_contract, RENDER_GRAPHICS_CONTRACT


def fixture():
    return {'kind':'compute','stages':[{'stage':'comp','entry_point':'main',
        'required_subgroup_size':None,'stage_flags':0}], 'layout':{'sets':[{'set':0,'flags':0,
        'bindings':[{'binding':i,'ty':7,'count':1,'stages':32,'flags':0,'immutable_sampler':None}
                    for i in range(2)]}], 'push_constants':[{'offset':0,'size':40,'stages':32}]}}


class LayoutTests(unittest.TestCase):
    def test_owned_compute_layout_and_optional_push_range(self):
        value=fixture();self.assertEqual(layout_contract(value),(2,40))
        value['layout']['push_constants']=[];self.assertEqual(layout_contract(value),(2,0))

    def test_wrong_stages_features_and_binding_contract_are_refused(self):
        changes=[(['kind'],'render'),(['stages'],[]),(['stages',0,'entry_point'],'other'),
            (['stages',0,'required_subgroup_size'],32),(['stages',0,'stage_flags'],1),
            (['layout','sets',0,'set'],1),(['layout','sets',0,'flags'],1),
            (['layout','sets',0,'bindings',0,'ty'],6),(['layout','sets',0,'bindings',0,'binding'],3),
            (['layout','sets',0,'bindings',0,'count'],2),(['layout','sets',0,'bindings',0,'flags'],1),
            (['layout','sets',0,'bindings',0,'immutable_sampler'],{}),
            (['layout','push_constants',0,'offset'],4),(['layout','push_constants',0,'size'],129),
            (['layout','push_constants',0,'stages'],1),(['layout','push_constants',0,'size'],3)]
        for path,replacement in changes:
            value=fixture();cursor=value
            for key in path[:-1]:cursor=cursor[key]
            cursor[path[-1]]=replacement
            with self.subTest(path=path),self.assertRaises(ValueError):layout_contract(value)


def render_fixture():
    return {'kind':'render','stages':[{'stage':name,'entry_point':'main',
        'required_subgroup_size':None,'stage_flags':0,'specialization':{'constants':[]}}
        for name in ('vert','frag')], 'layout':{'sets':[{'set':0,'bindings':[],'flags':0}],
        'push_constants':[],'push_descriptor_eligible':False},
        'vertex_input':{'bindings':[],'attributes':[],'pulled':[],'variant_pulled':[]},
        'graphics':copy.deepcopy(RENDER_GRAPHICS_CONTRACT)}


class RenderLayoutTests(unittest.TestCase):
    def test_supported_procedural_render_contract(self):
        render_contract(render_fixture())

    def test_specialization_bindings_and_raster_features_are_refused(self):
        changes=[(['stages',0,'stage'],'comp'),(['stages',1,'entry_point'],'other'),
            (['stages',0,'specialization'],{'constants':[{'id':0}]}),
            (['layout','sets',0,'bindings'],[{'binding':0}]),
            (['layout','push_constants'],[{'size':4}]),(['vertex_input','attributes'],[{}]),
            (['graphics','topology'],0),(['graphics','color_attachments',0,'format'],37),
            (['graphics','color_attachments',0,'blend'],{}),(['graphics','rasterization_samples'],4),
            (['graphics','declares_layer'],True),(['graphics','fragment_writes_memory'],True),
            (['graphics','logic_op'],3),(['graphics','new_unknown_feature'],True)]
        for path,replacement in changes:
            value=render_fixture();cursor=value
            for key in path[:-1]:cursor=cursor[key]
            cursor[path[-1]]=replacement
            with self.subTest(path=path),self.assertRaises(ValueError):render_contract(value)

    def test_malformed_render_shapes_fail_cleanly(self):
        for value in (None,[],{}, {'kind':'render','stages':[None,None]},
                {'kind':'render','stages':None}):
            with self.subTest(value=value),self.assertRaises(ValueError):render_contract(value)

    def test_malformed_json_shapes_fail_cleanly(self):
        for value in (None,[],{}, {'kind':'compute','stages':[None]},
                {'kind':'compute','stages':None}):
            with self.subTest(value=value),self.assertRaises(ValueError):layout_contract(value)
        for path in (['layout'],['layout','sets'],['layout','sets',0],
                ['layout','sets',0,'bindings'],['layout','sets',0,'bindings',0],
                ['layout','push_constants'],['layout','push_constants',0]):
            value=fixture();cursor=value
            for key in path[:-1]:cursor=cursor[key]
            cursor[path[-1]]=None
            with self.subTest(path=path),self.assertRaises(ValueError):layout_contract(value)


if __name__=='__main__':unittest.main(verbosity=2)
