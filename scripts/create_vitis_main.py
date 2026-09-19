"""Recreate and build the main application(s)."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
from vitis_apps import build_apps

build_apps('main', recreate=True)
