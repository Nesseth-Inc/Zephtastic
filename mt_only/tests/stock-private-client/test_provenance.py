"""No hardware: source export provenance must never discover an ancestor git."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec=importlib.util.spec_from_file_location('source_provenance',Path(__file__).with_name('provenance.py'))
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

class ExportProvenance(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.base=Path(self.temp.name);self.root=self.base/'source';self.root.mkdir()
  self.manifest=self.base/'SOURCE-MANIFEST.json';self.raw=json.dumps(dict(schema='zt-frozen-source-manifest/2',source_revision='a'*40,source_revision_kind='SHA-256 content declaration prefix, not a Git object ID'));self.manifest.write_text(self.raw)
 def tearDown(self):self.temp.cleanup()
 def test_parent_manifest_never_uses_git(self):
  with patch.object(module._core.subprocess,'check_output',side_effect=AssertionError('ancestor git forbidden')):
   result=module.source_provenance(self.root)
  self.assertEqual(result['source_revision'],'a'*40);self.assertEqual(result['source_revision_kind'],'SHA-256 content declaration prefix, not a Git object ID');self.assertNotIn('source_git_oid_sha1',result);self.assertIsNone(result['source_tree_dirty']);self.assertEqual(result['source_manifest_sha256'],module._core.hashlib.sha256(self.raw.encode()).hexdigest())
 def test_git_archive_manifest_labels_oid_truthfully(self):
  self.raw=json.dumps(dict(schema='zt-frozen-source-manifest/3',source_revision='d'*40,source_revision_kind='Git SHA-1 OID declared for this exact git-archive export'));self.manifest.write_text(self.raw)
  with patch.object(module._core.subprocess,'check_output',side_effect=AssertionError('ancestor git forbidden')):
   result=module.source_provenance(self.root)
  self.assertEqual(result['source_revision'],'d'*40);self.assertEqual(result['source_git_oid_sha1'],'d'*40);self.assertEqual(result['source_revision_kind'],'Git SHA-1 OID declared for this exact git-archive export')
 def test_local_manifest(self):
  self.manifest.rename(self.root/self.manifest.name);self.assertEqual(module.source_provenance(self.root)['source_revision'],'a'*40)
 def test_missing_manifest(self):
  self.manifest.unlink()
  with self.assertRaises(ValueError):module.source_provenance(self.root)
 def test_handoff_manifest_fallback(self):
  self.manifest.unlink();raw=json.dumps(dict(base_source='c'*40));handoff=self.base/'HANDOFF-MANIFEST.json';handoff.write_text(raw)
  result=module.source_provenance(self.root)
  self.assertEqual(result['source_revision'],'c'*40);self.assertEqual(result['source_provenance'],'handoff manifest base declaration; actual bytes separately hashed')
 def test_ambiguous_manifest(self):
  (self.root/self.manifest.name).write_text(self.raw)
  with self.assertRaises(ValueError):module.source_provenance(self.root)
 def test_invalid_revision(self):
  self.manifest.write_text(json.dumps(dict(schema='zt-frozen-source-manifest/2',source_revision='main',source_revision_kind='SHA-256 content declaration prefix, not a Git object ID')))
  with self.assertRaises(ValueError):module.source_provenance(self.root)
 def test_unknown_or_missing_revision_kind(self):
  for declaration in (dict(source_revision='a'*40),dict(source_revision='a'*40,source_revision_kind='invented')):
   self.manifest.write_text(json.dumps(declaration))
   with self.assertRaises(ValueError):module.source_provenance(self.root)
 def test_git_archive_kind_requires_v3_schema(self):
  self.manifest.write_text(json.dumps(dict(schema='zt-frozen-source-manifest/2',source_revision='a'*40,source_revision_kind='Git SHA-1 OID declared for this exact git-archive export')))
  with self.assertRaises(ValueError):module.source_provenance(self.root)
 def test_content_prefix_kind_requires_v2_schema(self):
  for schema in (None,'invented','zt-frozen-source-manifest/3'):
   declaration=dict(source_revision='a'*40,source_revision_kind='SHA-256 content declaration prefix, not a Git object ID')
   if schema is not None:declaration['schema']=schema
   self.manifest.write_text(json.dumps(declaration))
   with self.assertRaises(ValueError):module.source_provenance(self.root)
 def test_repo_local_git(self):
  (self.root/'.git').write_text('gitdir: actual-worktree')
  with patch.object(module._core.subprocess,'check_output',side_effect=['b'*40+'\n',' M changed\n']) as git:
   result=module.source_provenance(self.root)
  self.assertEqual(result['source_git_oid_sha1'],'b'*40);self.assertTrue(result['source_tree_dirty']);self.assertEqual(git.call_count,2)

if __name__=='__main__':unittest.main()
