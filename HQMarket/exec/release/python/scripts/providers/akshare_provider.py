"""Thin AKShare adapter for instruments and historical daily bars."""
from __future__ import annotations

import re

try:
    from pypinyin import Style, lazy_pinyin
except ImportError:
    Style = None
    lazy_pinyin = None


_SECURITY_PREFIX_PATTERN = re.compile(
    r"^(?:S[＊*]ST|SST|[＊*]ST|ST|N|C|U|W|V)+",
    re.IGNORECASE,
)


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
        import akshare as ak
        frame = ak.stock_zh_a_hist(symbol=symbol, period="daily", start_date=start, end_date=end, adjust=adjustment)
        if frame is None:
            return []
        frame = frame.rename(columns={
            "日期": "date", "开盘": "open", "最高": "high", "最低": "low", "收盘": "close",
            "成交量": "volume", "成交额": "turnover",
        })
        frame["date"] = frame["date"].astype(str)
        return frame.to_dict(orient="records")

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
