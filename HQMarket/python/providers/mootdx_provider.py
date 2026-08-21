"""Thin MooTDX adapter. C++ owns subscriptions, retry policy and caching."""
from __future__ import annotations

from typing import Iterable


class MooTdxProvider:
    def __init__(self) -> None:
        self._client = None

    def initialize(self) -> None:
        from mootdx.quotes import Quotes
        self._client = Quotes.factory(market="std", multithread=True, heartbeat=True)

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
