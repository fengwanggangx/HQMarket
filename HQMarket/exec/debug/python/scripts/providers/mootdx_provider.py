"""Thin MooTDX adapter. C++ owns subscriptions, retry policy and caching."""
from __future__ import annotations

import json
from pathlib import Path
from typing import Iterable
from urllib.request import Request, urlopen


_MOOTDX_BATCH_SIZE = 80
_TENCENT_BATCH_SIZE = 60


def _ensure_mootdx_config() -> tuple[str, int]:
    """Create a usable config without mootdx's slow first-run server scan."""
    from mootdx import config
    from mootdx.consts import HQ_HOSTS

    config_path = Path(config.CONF)
    try:
        options = json.loads(config_path.read_text(encoding="utf-8"))
        if not options.get("SERVER", {}).get("HQ"):
            raise ValueError("missing HQ servers")
    except (OSError, ValueError, TypeError, json.JSONDecodeError):
        config_path.parent.mkdir(parents=True, exist_ok=True)
        config_path.write_text(
            json.dumps(config.clone(), indent=2, ensure_ascii=False),
            encoding="utf-8",
        )

    _, address, port = HQ_HOSTS[0]
    return address, port


class MooTdxProvider:
    def __init__(self) -> None:
        self._client = None
        self._use_tencent = False

    def initialize(self) -> None:
        from mootdx.quotes import Quotes
        self._client = Quotes.factory(
            market="std",
            server=_ensure_mootdx_config(),
            timeout=3,
            # Quotes are polled every 800 ms by C++, so tdxpy's background
            # heartbeat is unnecessary.  It also races with quote reads on the
            # same socket and logs a full traceback when a server disconnects.
            heartbeat=False,
            auto_retry=False,
            raise_exception=True,
        )

    def quotes(self, instruments: Iterable[dict]) -> list[dict]:
        values = list(instruments)
        if not values:
            return []
        if self._client is None and not self._use_tencent:
            raise RuntimeError("MooTDX provider is not initialized")
        if not self._use_tencent:
            try:
                return self._mootdx_quotes(values)
            except Exception:
                self._client.close()
                self._client = None
                self._use_tencent = True
        return self._tencent_quotes(values)

    def _mootdx_quotes(self, instruments: list[dict]) -> list[dict]:
        rows: list[dict] = []
        for offset in range(0, len(instruments), _MOOTDX_BATCH_SIZE):
            batch = instruments[offset:offset + _MOOTDX_BATCH_SIZE]
            symbols = [(self._market(item["exchange"]), item["symbol"]) for item in batch]
            result = self._client.client.get_security_quotes(symbols)
            if not result:
                raise RuntimeError("MooTDX returned no quote data")
            for item, row in zip(batch, result):
                row["symbol"] = item["symbol"]
                row["exchange"] = item["exchange"]
                row["source"] = "mootdx"
                rows.append(row)
        return rows

    def _tencent_quotes(self, instruments: list[dict]) -> list[dict]:
        rows: list[dict] = []
        for offset in range(0, len(instruments), _TENCENT_BATCH_SIZE):
            batch = instruments[offset:offset + _TENCENT_BATCH_SIZE]
            codes = [self._tencent_code(item) for item in batch]
            request = Request(
                "https://qt.gtimg.cn/q=" + ",".join(codes),
                headers={"Referer": "https://gu.qq.com/", "User-Agent": "Mozilla/5.0"},
            )
            with urlopen(request, timeout=8) as response:
                content = response.read().decode("gb18030")
            values = self._parse_tencent(content)
            for item, code in zip(batch, codes):
                fields = values.get(code)
                if fields is not None:
                    rows.append(self._tencent_row(item, fields))
        if not rows:
            raise RuntimeError("Tencent returned no quote data")
        return rows

    @staticmethod
    def _tencent_code(item: dict) -> str:
        prefixes = {"SSE": "sh", "SZSE": "sz", "BSE": "bj"}
        try:
            return prefixes[item["exchange"]] + item["symbol"]
        except KeyError as error:
            raise ValueError(f"unsupported Tencent exchange: {item.get('exchange')}") from error

    @staticmethod
    def _parse_tencent(content: str) -> dict[str, list[str]]:
        values: dict[str, list[str]] = {}
        for line in content.split(";"):
            if '="' not in line:
                continue
            name, value = line.split('="', 1)
            code = name.strip().removeprefix("v_")
            values[code] = value.rstrip('"\r\n').split("~")
        return values

    @staticmethod
    def _number(fields: list[str], index: int) -> float:
        try:
            return float(fields[index])
        except (IndexError, TypeError, ValueError):
            return 0.0

    @classmethod
    def _tencent_row(cls, item: dict, fields: list[str]) -> dict:
        amount_parts = fields[35].split("/") if len(fields) > 35 else []
        row = {
            "symbol": item["symbol"],
            "exchange": item["exchange"],
            "source": "tencent",
            "price": cls._number(fields, 3),
            "last_close": cls._number(fields, 4),
            "open": cls._number(fields, 5),
            "vol": int(cls._number(fields, 6)),
            "high": cls._number(fields, 33),
            "low": cls._number(fields, 34),
            "amount": float(amount_parts[2]) if len(amount_parts) > 2 else 0.0,
        }
        for level in range(1, 6):
            bid_index = 9 + (level - 1) * 2
            ask_index = 19 + (level - 1) * 2
            row[f"bid{level}"] = cls._number(fields, bid_index)
            row[f"bid_vol{level}"] = int(cls._number(fields, bid_index + 1))
            row[f"ask{level}"] = cls._number(fields, ask_index)
            row[f"ask_vol{level}"] = int(cls._number(fields, ask_index + 1))
        return row

    @staticmethod
    def _market(exchange: str) -> int:
        if exchange == "SSE":
            return 1
        if exchange in {"SZSE", "BSE"}:
            return 0
        raise ValueError(f"unsupported MooTDX exchange: {exchange}")
