from __future__ import annotations

import threading
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image

try:
    from ai_edge_litert.interpreter import Interpreter
except ImportError:  # pragma: no cover - older/alternate install
    from tflite_runtime.interpreter import Interpreter


class DefectClassifier:

    def __init__(self, model_path: str, labels: tuple[str, str], input_size: int) -> None:
        self.labels = labels
        self.input_size = input_size
        self._lock = threading.Lock()

        self.interpreter = Interpreter(model_path=model_path)
        self.interpreter.allocate_tensors()
        self._input_index = self.interpreter.get_input_details()[0]["index"]
        self._output_index = self.interpreter.get_output_details()[0]["index"]

    def _preprocess(self, path: Path) -> np.ndarray:
        image = Image.open(path).convert("RGB")

        width, height = image.size
        side = min(width, height)
        left = (width - side) // 2
        top = (height - side) // 2
        image = image.crop((left, top, left + side, top + side))
        image = image.resize((self.input_size, self.input_size), Image.NEAREST)

        array = np.asarray(image, dtype=np.float32)
        array = (array / 127.5) - 1.0
        return np.expand_dims(array, axis=0)

    def classify(self, path: Path) -> dict[str, Any]:
        tensor = self._preprocess(path)
        with self._lock:
            self.interpreter.set_tensor(self._input_index, tensor)
            self.interpreter.invoke()
            output = self.interpreter.get_tensor(self._output_index)[0]

        scores = {label: float(score) for label, score in zip(self.labels, output)}
        label = self.labels[int(np.argmax(output))]
        return {
            "label": label,
            "confidence": scores[label],
            "scores": scores,
        }
