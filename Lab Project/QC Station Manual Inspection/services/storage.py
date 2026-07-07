from __future__ import annotations

import json
from datetime import datetime
from pathlib import Path
from typing import Any


class InspectionStorage:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.root.mkdir(parents=True, exist_ok=True)

    @staticmethod
    def new_id() -> str:
        return datetime.now().strftime("QC-%Y%m%d-%H%M%S-%f")[:-3]

    def directory(self, inspection_id: str) -> Path:
        return self.root / inspection_id

    def image_path(self, inspection_id: str, view: int, angle: int) -> Path:
        return self.directory(inspection_id) / f"view_{view}_{angle:03d}deg.jpg"

    def save(self, record: dict[str, Any]) -> None:
        directory = self.directory(record["inspection_id"])
        directory.mkdir(parents=True, exist_ok=True)
        path = directory / "result.json"
        temporary = directory / "result.json.tmp"
        temporary.write_text(json.dumps(record, indent=2), encoding="utf-8")
        temporary.replace(path)
