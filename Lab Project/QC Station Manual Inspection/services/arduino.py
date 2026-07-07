from __future__ import annotations

from typing import Any

import requests


class ArduinoClient:
    def __init__(self, base_url: str, timeout: int = 5) -> None:
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.online: bool | None = None
        self.angle = 0

    def _get(self, route: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        try:
            response = requests.get(
                f"{self.base_url}{route}",
                params=params,
                timeout=self.timeout,
            )
            response.raise_for_status()
            payload = response.json()
            self.online = True
            return payload
        except (requests.RequestException, ValueError) as exc:
            self.online = False
            raise RuntimeError(f"Arduino request failed: {exc}") from exc

    def move(self, angle: int) -> None:
        payload = self._get("/move", {"angle": angle})
        if payload.get("angle") != angle:
            raise RuntimeError("Arduino returned the wrong servo angle")
        self.angle = angle

    def home(self) -> None:
        self._get("/home")
        self.angle = 0
