"""Truthful source identity for a Git checkout or a manifest-bound export."""
import hashlib
import json
import os
import re
import stat
import subprocess
from pathlib import Path, PurePosixPath


def _regular_file(path, limit):
    try:
        info = path.lstat()
    except OSError as exc:
        raise ValueError('Selected export input is unavailable') from exc
    if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_size < 0 or info.st_size > limit:
        raise ValueError('Selected export inputs must be bounded, singly-linked regular files')
    return path.read_bytes()


def _selected_export_provenance(root, manifest_path, declaration, raw):
    """Verify the M186 selected-export declaration without claiming full-tree membership."""
    expected_keys = {
        'files', 'schema', 'scope', 'source_branch', 'source_epoch',
        'source_revision', 'source_tree',
    }
    if set(declaration) != expected_keys or declaration.get('schema') != 'zt-m186-selected-source/v1':
        raise ValueError('Unsupported selected-source manifest schema')
    revision = declaration.get('source_revision')
    tree = declaration.get('source_tree')
    if not isinstance(revision, str) or not re.fullmatch(r'[0-9a-f]{40}', revision):
        raise ValueError('Selected-source manifest needs a full Git commit SHA-1')
    if not isinstance(tree, str) or not re.fullmatch(r'[0-9a-f]{40}', tree):
        raise ValueError('Selected-source manifest needs a full declared tree SHA-1')
    if (not isinstance(declaration.get('source_branch'), str) or
            not isinstance(declaration.get('scope'), str) or
            type(declaration.get('source_epoch')) is not int or declaration['source_epoch'] < 0):
        raise ValueError('Selected-source manifest metadata is malformed')
    if declaration['scope'] != (
            'All root files plus docs, mt_only and zephcore; excludes historical checkpoints/artifacts/'
            'controllers/lab/validation/history, no product source omitted from selected build scope'):
        raise ValueError('Selected-source scope declaration is not the reviewed M186 scope')

    commit_path = manifest_path.parent / 'SOURCE-COMMIT.raw'
    commit = _regular_file(commit_path, 1024 * 1024)
    git_commit_oid = hashlib.sha1(b'commit ' + str(len(commit)).encode('ascii') + b'\0' + commit).hexdigest()
    if git_commit_oid != revision:
        raise ValueError('Raw Git commit object does not match source_revision')
    header = commit.split(b'\n\n', 1)[0]
    roots = [line[5:].decode('ascii', errors='strict') for line in header.splitlines()
             if line.startswith(b'tree ')]
    if roots != [tree]:
        raise ValueError('Declared source_tree does not match the verified commit header')

    rows = declaration.get('files')
    if not isinstance(rows, list) or not rows or len(rows) > 50000:
        raise ValueError('Selected-source file inventory is missing or out of bounds')
    expected_paths = set()
    expected_rows = {}
    for row in rows:
        if not isinstance(row, dict) or set(row) != {
                'bytes', 'git_blob_oid', 'mode', 'path', 'public_sha256'}:
            raise ValueError('Selected-source inventory row has an unexpected shape')
        name = row.get('path')
        if (not isinstance(name, str) or not name or '\\' in name or '\x00' in name):
            raise ValueError('Selected-source path is malformed')
        relative = PurePosixPath(name)
        if (relative.is_absolute() or relative.as_posix() != name or
                any(part in ('', '.', '..') for part in relative.parts)):
            raise ValueError('Selected-source path escapes its export root')
        if len(relative.parts) > 1 and relative.parts[0] not in ('docs', 'mt_only', 'zephcore'):
            raise ValueError('Selected-source path is outside the declared package scope')
        if name in expected_paths:
            raise ValueError('Selected-source inventory contains a duplicate path')
        if (type(row.get('bytes')) is not int or row['bytes'] < 0 or row['bytes'] > 32 * 1024 * 1024 or
                type(row.get('mode')) is not int or row['mode'] not in (0o644, 0o755) or
                not isinstance(row.get('git_blob_oid'), str) or
                not re.fullmatch(r'[0-9a-f]{40}', row['git_blob_oid']) or
                not isinstance(row.get('public_sha256'), str) or
                not re.fullmatch(r'[0-9a-f]{64}', row['public_sha256'])):
            raise ValueError('Selected-source file metadata is malformed')
        expected_paths.add(name)
        expected_rows[name] = row

    actual_paths = set()
    for current, dirs, files in os.walk(root, topdown=True, followlinks=False):
        current_path = Path(current)
        for name in list(dirs):
            child = current_path / name
            if child.is_symlink() or not child.is_dir():
                raise ValueError('Selected-source export contains a non-directory or symlink directory')
        for name in files:
            child = current_path / name
            rel = child.relative_to(root).as_posix()
            body = _regular_file(child, 32 * 1024 * 1024)
            info = child.lstat()
            if stat.S_IMODE(info.st_mode) not in (0o644, 0o755):
                raise ValueError('Selected-source file has an unsupported mode')
            actual_paths.add(rel)
            row = expected_rows.get(rel)
            if row is None:
                raise ValueError('Selected-source export has an undeclared file')
            blob_oid = hashlib.sha1(b'blob ' + str(len(body)).encode('ascii') + b'\0' + body).hexdigest()
            if (len(body) != row['bytes'] or hashlib.sha256(body).hexdigest() != row['public_sha256'] or
                    blob_oid != row['git_blob_oid'] or stat.S_IMODE(info.st_mode) != row['mode']):
                raise ValueError('Selected-source file bytes, Git blob OID, or mode disagree with manifest')
    if actual_paths != expected_paths:
        raise ValueError('Selected-source export inventory is incomplete')

    manifest_sha = hashlib.sha256(raw).hexdigest()
    return dict(
        source_revision=revision,
        source_commit_oid_sha1=git_commit_oid,
        source_tree=tree,
        source_tree_dirty=None,
        source_provenance=(
            'M186 sparse selected-export declaration; raw Git commit object and root-tree reference '
            'verified; selected file inventory, bytes, modes and Git blob OIDs verified; '
            'full Git-tree membership not independently proven'),
        source_revision_kind=(
            'verified Git commit with manifest-declared sparse export; selected contents verified, '
            'not a full git-archive or independent commit-tree membership proof'),
        selected_export_schema=declaration['schema'],
        selected_export_files=len(rows),
        selected_export_inventory_verified=True,
        full_git_tree_membership_proven=False,
        source_manifest_sha256=manifest_sha,
    )


def source_provenance(root):
    root = Path(root).resolve()
    if (root / '.git').exists():
        revision = subprocess.check_output(
            ['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
        if not re.fullmatch('[0-9a-f]{40}', revision):
            raise ValueError('Git source needs a full SHA-1 object ID')
        dirty = bool(subprocess.check_output(
            ['git', '-C', str(root), 'status', '--porcelain', '-uall'], text=True).strip())
        return dict(source_git_oid_sha1=revision, source_tree_dirty=dirty,
                    source_provenance='repo-local .git')
    manifests = [p for p in (root / 'SOURCE-MANIFEST.json', root.parent / 'SOURCE-MANIFEST.json')
                 if p.is_file()]
    revision_key = 'source_revision'
    provenance = 'export manifest declaration; actual bytes separately hashed'
    if not manifests and (root.parent / 'HANDOFF-MANIFEST.json').is_file():
        manifests = [root.parent / 'HANDOFF-MANIFEST.json']
        revision_key = 'base_source'
        provenance = 'handoff manifest base declaration; actual bytes separately hashed'
    if len(manifests) != 1:
        raise ValueError('Export needs exactly one local/parent SOURCE-MANIFEST.json or a parent HANDOFF-MANIFEST.json fallback')
    raw = manifests[0].read_bytes()
    declaration = json.loads(raw)
    revision = declaration.get(revision_key)
    if not isinstance(revision, str) or not re.fullmatch('[0-9a-f]{40}', revision):
        raise ValueError(f'{manifests[0].name} needs a full {revision_key}')
    result = dict(source_revision=revision,
                  source_tree_dirty=None,
                  source_provenance=provenance,
                  source_manifest_sha256=hashlib.sha256(raw).hexdigest())
    if revision_key == 'base_source':
        result['source_revision_kind'] = 'handoff manifest base declaration, not independently proven as a Git object'
        return result
    kind = declaration.get('source_revision_kind')
    if declaration.get('schema') == 'zt-m186-selected-source/v1':
        if revision_key != 'source_revision':
            raise ValueError('Selected-source manifest must use source_revision')
        return _selected_export_provenance(root, manifests[0], declaration, raw)
    git_kind = 'Git SHA-1 OID declared for this exact git-archive export'
    content_kind = 'SHA-256 content declaration prefix, not a Git object ID'
    if kind == git_kind:
        if declaration.get('schema') != 'zt-frozen-source-manifest/3':
            raise ValueError('Git archive source declaration needs zt-frozen-source-manifest/3')
        result['source_git_oid_sha1'] = revision
    elif kind == content_kind:
        if declaration.get('schema') != 'zt-frozen-source-manifest/2':
            raise ValueError('Content-prefix source declaration needs zt-frozen-source-manifest/2')
    else:
        raise ValueError('SOURCE-MANIFEST.json has an unknown or missing source_revision_kind')
    result['source_revision_kind'] = kind
    return result
