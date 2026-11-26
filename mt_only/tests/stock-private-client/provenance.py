"""Compatibility import for the single source-provenance implementation."""
import importlib.util
from pathlib import Path

_path=Path(__file__).resolve().parents[3]/'source_provenance.py'
_spec=importlib.util.spec_from_file_location('m22_source_provenance',_path)
_core=importlib.util.module_from_spec(_spec);_spec.loader.exec_module(_core)
source_provenance=_core.source_provenance
