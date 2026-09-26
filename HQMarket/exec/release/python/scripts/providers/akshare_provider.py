"""Thin AKShare adapter for instruments and historical bars."""
from __future__ import annotations

import re
import json
from datetime import datetime
from urllib.parse import urlencode
from urllib.request import Request, urlopen
from time import sleep

try:
    from pypinyin import Style, lazy_pinyin
except ImportError:
    Style = None
    lazy_pinyin = None


_SECURITY_PREFIX_PATTERN = re.compile(
    r"^(?:S[＊*]ST|SST|[＊*]ST|ST|N|C|U|W|V)+",
    re.IGNORECASE,
)


def _tencent_code(symbol: str) -> str:
    if symbol.startswith("6"):
        return "sh" + symbol
    if symbol.startswith(("0", "3")):
        return "sz" + symbol
    return "bj" + symbol


def _tencent_json(path: str, parameters: dict[str, str]) -> dict:
    request = Request(
        "https://web.ifzq.gtimg.cn" + path + "?" + urlencode(parameters, safe=","),
        headers={"Referer": "https://gu.qq.com/", "User-Agent": "Mozilla/5.0"},
    )
    for attempt in range(2):
        try:
            with urlopen(request, timeout=10) as response:
                return json.loads(response.read().decode("utf-8"))
        except Exception:
            if attempt > 0:
                raise
            sleep(0.25)
    raise RuntimeError("Tencent history request failed")


def _tencent_daily_bars(symbol: str, start: str, end: str) -> list[dict]:
    code = _tencent_code(symbol)
    start = datetime.strptime(start, "%Y%m%d").strftime("%Y-%m-%d")
    end = datetime.strptime(end, "%Y%m%d").strftime("%Y-%m-%d")
    response = _tencent_json(
        "/appstock/app/fqkline/get",
        {"param": f"{code},day,{start},{end},640,qfq"},
    )
    data = response.get("data", {})
    value = data.get(code, {}) if isinstance(data, dict) else {}
    rows = value.get("qfqday") or value.get("day") or []
    return [
        {
            "date": row[0], "open": row[1], "close": row[2],
            "high": row[3], "low": row[4], "volume": row[5], "turnover": 0,
        }
        for row in rows if len(row) >= 6
    ]


def _tencent_minute_bars(symbol: str) -> list[dict]:
    code = _tencent_code(symbol)
    response = _tencent_json(
        "/appstock/app/day/query", {"code": code}
    )
    data = response.get("data", {})
    value = data.get(code, {}).get("data", []) if isinstance(data, dict) else []
    bars: list[dict] = []
    for day in value:
        date = str(day.get("date", ""))
        if len(date) != 8:
            continue
        previous_volume = 0.0
        previous_turnover = 0.0
        for text in day.get("data", []):
            fields = str(text).split()
            if len(fields) < 4:
                continue
            price = float(fields[1])
            cumulative_volume = float(fields[2])
            cumulative_turnover = float(fields[3])
            bars.append({
                "date": datetime.strptime(date + fields[0], "%Y%m%d%H%M").strftime("%Y-%m-%d %H:%M:%S"),
                "open": price, "high": price, "low": price, "close": price,
                "volume": max(0.0, cumulative_volume - previous_volume),
                "turnover": max(0.0, cumulative_turnover - previous_turnover),
            })
            previous_volume = cumulative_volume
            previous_turnover = cumulative_turnover
    return bars


def _normalize_pinyin(values: list[str]) -> str:
    return re.sub(r"[^a-z0-9]", "", "".join(values).lower())


def _convert_pinyin(name: str) -> tuple[str, str]:
    if Style is None or lazy_pinyin is None:
        return "", ""
    full = _normalize_pinyin(lazy_pinyin(name, style=Style.NORMAL))
    short = _normalize_pinyin(lazy_pinyin(name, style=Style.FIRST_LETTER))
    return full, short


def make_pinyin_aliases(name: str) -> tuple[list[str], list[str]]:
    normalized_name = name.strip()
    plain_name = _SECURITY_PREFIX_PATTERN.sub("", normalized_name)
    full_aliases: list[str] = []
    short_aliases: list[str] = []
    for candidate in (normalized_name, plain_name):
        if not candidate:
            continue
        full, short = _convert_pinyin(candidate)
        if full and full not in full_aliases:
            full_aliases.append(full)
        if short and short not in short_aliases:
            short_aliases.append(short)
    return full_aliases, short_aliases


class AkShareProvider:
    def instruments(self) -> list[dict]:
        import akshare as ak
        frame = ak.stock_info_a_code_name()
        records = frame.rename(columns={"code": "symbol", "name": "name"}).to_dict(orient="records")
        for item in records:
            full_aliases, short_aliases = make_pinyin_aliases(str(item.get("name", "")))
            item["pinyin_full_aliases"] = full_aliases
            item["pinyin_short_aliases"] = short_aliases
        return records

    def daily_bars(self, symbol: str, start: str, end: str, adjustment: str = "") -> list[dict]:
        try:
            import akshare as ak
            frame = ak.stock_zh_a_hist(symbol=symbol, period="daily", start_date=start, end_date=end, adjust=adjustment)
            if frame is not None and not frame.empty:
                frame = frame.rename(columns={
                    "日期": "date", "开盘": "open", "最高": "high", "最低": "low", "收盘": "close",
                    "成交量": "volume", "成交额": "turnover",
                })
                frame["date"] = frame["date"].astype(str)
                return frame.to_dict(orient="records")
        except Exception:
            pass
        return _tencent_daily_bars(symbol, start, end)

    def minute_bars(self, symbol: str, start: str, end: str, adjustment: str = "") -> list[dict]:
        try:
            import akshare as ak
            frame = ak.stock_zh_a_hist_min_em(
                symbol=symbol, start_date=start, end_date=end, period="1", adjust=adjustment
            )
            if frame is not None and not frame.empty:
                frame = frame.rename(columns={
                    "时间": "date", "开盘": "open", "最高": "high", "最低": "low", "收盘": "close",
                    "成交量": "volume", "成交额": "turnover",
                })
                frame["date"] = frame["date"].astype(str)
                return frame.to_dict(orient="records")
        except Exception:
            pass
        return _tencent_minute_bars(symbol)

    def industry_sectors(self) -> list[dict]:
        import akshare as ak
        frame = ak.stock_board_industry_name_em()
        if frame is None:
            return []
        frame = frame.rename(columns={
            "板块代码": "code", "板块名称": "name", "涨跌幅": "change_percent",
            "上涨家数": "rising_count", "下跌家数": "falling_count",
            "领涨股票": "leading_name",
        })
        frame["flat_count"] = 0
        frame["member_count"] = frame.get("rising_count", 0) + frame.get("falling_count", 0)
        frame["leading_symbol"] = ""
        columns = ["code", "name", "change_percent", "rising_count", "falling_count",
                   "flat_count", "member_count", "leading_name", "leading_symbol"]
        return frame.reindex(columns=columns).fillna(0).to_dict(orient="records")

    def industry_constituents(self, sector_name: str) -> list[dict]:
        import akshare as ak
        frame = ak.stock_board_industry_cons_em(symbol=sector_name)
        if frame is None:
            return []
        frame = frame.rename(columns={"代码": "symbol", "名称": "name"})
        return frame.reindex(columns=["symbol", "name"]).fillna("").to_dict(orient="records")
