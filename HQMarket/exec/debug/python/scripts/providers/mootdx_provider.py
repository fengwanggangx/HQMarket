"""Thin MooTDX adapter. C++ owns subscriptions, retry policy and caching."""
from __future__ import annotations

import json
from typing import Iterable


def _ensure_mootdx_config() -> tuple[str, int]:
    """Create a usable config without mootdx's slow first-run server scan."""
    from mootdx import config
    from mootdx.consts import HQ_HOSTS

    try:
        options = json.loads(config.CONF.read_text(encoding="utf-8"))
        if not options.get("SERVER", {}).get("HQ"):
            raise ValueError("missing HQ servers")
    except (OSError, ValueError, TypeError, json.JSONDecodeError):
        config.CONF.parent.mkdir(parents=True, exist_ok=True)
        config.CONF.write_text(
            json.dumps(config.clone(), indent=2, ensure_ascii=False),
            encoding="utf-8",
        )

    _, address, port = HQ_HOSTS[0]
    return address, port


class MooTdxProvider:
    def __init__(self) -> None:
        self._client = None

    def initialize(self) -> None:
        from mootdx.quotes import Quotes
        self._client = Quotes.factory(
            market="std",
            server=_ensure_mootdx_config(),
            timeout=3,
            multithread=True,
            heartbeat=True,
        )

    def quotes(self, instruments: Iterable[dict]) -> list[dict]:
        if self._client is None:
            raise RuntimeError("MooTDX provider is not initialized")
        symbols = [(self._market(item["exchange"]), item["symbol"]) for item in instruments]
        frame = self._client.quotes(symbol=symbols)
        return [] if frame is None else frame.to_dict(orient="records")

    @staticmethod
    def _market(exchange: str) -> int:
        if exchange == "SSE":
            return 1
        if exchange in {"SZSE", "BSE"}:
            return 0
        raise ValueError(f"unsupported MooTDX exchange: {exchange}")
