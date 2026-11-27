"""Thin AKShare adapter for instruments and historical bars."""
from __future__ import annotations

import csv
import re
import json
import os
import sys
from datetime import datetime, timedelta
from pathlib import Path
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


def _tencent_code(symbol: str, exchange: str = "") -> str:
    if exchange == "HKEX":
        return "hk" + symbol
    if exchange == "SSE":
        return "sh" + symbol
    if exchange == "SZSE":
        return "sz" + symbol
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


def _tencent_daily_bars(symbol: str, start: str, end: str, exchange: str = "") -> list[dict]:
    code = _tencent_code(symbol, exchange)
    first = datetime.strptime(start, "%Y%m%d").date()
    last = datetime.strptime(end, "%Y%m%d").date()
    bars = {}
    while first <= last:
        page_end = first + timedelta(days=min(639, (last - first).days))
        response = _tencent_json(
            "/appstock/app/fqkline/get",
            {"param": f"{code},day,{first},{page_end},640,"},
        )
        if not isinstance(response, dict) or response.get("code") not in (0, "0"):
            raise RuntimeError(f"Tencent daily history failed for {code} on {first}")
        data = response.get("data")
        series = data.get(code) if isinstance(data, dict) else None
        rows = series.get("day") if isinstance(series, dict) else None
        if not isinstance(rows, list):
            raise RuntimeError(f"Invalid Tencent daily history page for {code} on {first}")
        for row in rows:
            if len(row) >= 6 and first.isoformat() <= row[0] <= page_end.isoformat():
                bars[row[0]] = {
                    "source": "tencent", "date": row[0], "open": row[1], "close": row[2],
                    "high": row[3], "low": row[4], "volume": row[5],
                }
        if page_end == last:
            break
        first = page_end + timedelta(days=1)
    return [bars[date] for date in sorted(bars)]


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


def _local_directory(path: Path, exchange: str, kind: str) -> list[dict]:
    records = {}
    code_pattern = r"[0-9]{5}" if exchange == "HKEX" else r"[0-9]{6}"
    with path.open(encoding="utf-8-sig", newline="") as source:
        for row in csv.reader(source, strict=True):
            if len(row) != 2:
                raise ValueError(f"Expected security code and name in {path}")
            code, name = (value.strip() for value in row)
            if not re.fullmatch(code_pattern, code) or not name or code in records:
                raise ValueError(f"Invalid or duplicate {exchange} security: {code}")
            records[code] = {"symbol": code, "name": name, "exchange": exchange, "kind": kind}
    if not records:
        raise ValueError(f"Empty {exchange} directory: {path}")
    return list(records.values())


def _sse_fund_directory(fund_type: str) -> list[dict]:
    parameters = {"sqlId": "FUND_LIST", "isPagination": "true", "fundType": fund_type,
                  "pageHelp.pageSize": "100", "pageHelp.cacheSize": "1", "pagecache": "false"}
    records = {}
    page_number = 1
    expected_total = None
    while True:
        for key in ("pageNo", "beginPage", "endPage"):
            parameters["pageHelp." + key] = str(page_number)
        request = Request(
            "https://query.sse.com.cn/commonSoaQuery.do?" + urlencode(parameters),
            headers={"Referer": "https://www.sse.com.cn/assortment/fund/list/", "User-Agent": "Mozilla/5.0"},
        )
        with urlopen(request, timeout=10) as response:
            page = json.load(response)["pageHelp"]
        total = int(page["total"])
        if expected_total is not None and total != expected_total:
            raise ValueError("SSE fund directory total changed during pagination")
        expected_total = total
        for row in page["data"]:
            code = str(row["fundCode"])
            name = str(row["fundAbbr"]).strip()
            if not re.fullmatch(r"[0-9]{6}", code) or not name or code in records:
                raise ValueError(f"Invalid or duplicate SSE fund: {code}")
            records[code] = {"symbol": code, "name": name, "exchange": "SSE", "kind": "fund"}
        if page_number >= int(page["pageCount"]):
            break
        page_number += 1
    if len(records) != expected_total or (fund_type in {"00", "10", "50"} and not records):
        raise ValueError("Incomplete SSE fund directory")
    return list(records.values())


class AkShareProvider:
    def instruments(self) -> list[dict]:
        import akshare as ak
        for fetch in (ak.stock_info_a_code_name, ak.stock_info_sh_name_code,
                      ak.stock_info_sz_name_code, ak.stock_info_bj_name_code):
            clear_cache = getattr(fetch, "cache_clear", None)
            if clear_cache is not None:
                clear_cache()
        for attempt in range(3):
            try:
                frame = ak.stock_info_a_code_name()
                if frame.empty:
                    raise ValueError("empty A-share directory returned by AKShare")
                break
            except (OSError, ValueError) as error:
                if attempt == 2:
                    raise
                print(f"AKShare A-share directory attempt {attempt + 1} failed: {type(error).__name__}: {error}",
                      file=sys.stderr)
                sleep(2 ** attempt)
        records = frame.rename(columns={"code": "symbol", "name": "name"}).to_dict(orient="records")
        for record in records:
            record["kind"] = "a_share"
        try:
            indices = ak.stock_zh_index_spot_sina()
            for row in indices.to_dict(orient="records"):
                code = str(row["代码"]).lower()
                if (code.startswith("sh000") or code.startswith("sz399")) and len(code) == 8:
                    records.append({"symbol": code[2:], "name": str(row["名称"]),
                                    "exchange": "SSE" if code.startswith("sh") else "SZSE",
                                    "kind": "index"})
        except Exception as error:
            print(f"akshare stock_zh_index_spot_sina failed: {type(error).__name__}: {error}", file=sys.stderr)
        try:
            hong_kong = ak.stock_hk_spot()
            for row in hong_kong.to_dict(orient="records"):
                code = str(row["代码"]).zfill(5)
                if code.isdigit():
                    records.append({"symbol": code, "name": str(row["中文名称"]), "exchange": "HKEX"})
        except Exception as error:
            print(f"akshare stock_hk_spot failed: {type(error).__name__}: {error}", file=sys.stderr)
        if os.environ.get("HQMARKET_ENABLE_MARKET_SCAN") == "1":
            try:
                existing_hk = {item["symbol"] for item in records if item.get("exchange") == "HKEX"}
                records.extend(item for item in _local_directory(Path("ini") / "hk_supplement.csv", "HKEX", "unknown")
                               if item["symbol"] not in existing_hk)
            except FileNotFoundError:
                pass
            except (OSError, ValueError, csv.Error) as error:
                print(f"HKEX supplemental directory failed: {type(error).__name__}: {error}", file=sys.stderr)
            records.append({"symbol": "899050", "name": "北证50", "exchange": "BSE", "kind": "index"})
            for exchange, filename in (("SSE", "sse_indices.csv"), ("SZSE", "szse_indices.csv"), ("BSE", "bse_indices.csv")):
                path = Path("ini") / filename
                try:
                    existing_indices = {item["symbol"] for item in records
                                        if item.get("kind") == "index" and item.get("exchange") == exchange}
                    records.extend(item for item in _local_directory(path, exchange, "index")
                                   if item["symbol"] not in existing_indices)
                except FileNotFoundError:
                    pass
                except (OSError, ValueError, csv.Error) as error:
                    print(f"{exchange} index directory {path} failed: {type(error).__name__}: {error}", file=sys.stderr)
            official_sse_sources = set()
            for source, fund_type in (("fund_etf_spot_em", "00"), ("fund_lof_spot_em", "10"),
                                      ("closed_end_fund", "40"), ("reits", "50")):
                try:
                    records.extend(_sse_fund_directory(fund_type))
                    official_sse_sources.add(source)
                except Exception as error:
                    print(f"SSE fund directory {fund_type} failed: {type(error).__name__}: {error}", file=sys.stderr)
            local_szse_sources = set()
            for source, filename in (("fund_etf_spot_em", "szse_etf.csv"),
                                     ("fund_lof_spot_em", "szse_lof.csv"), ("reits", "szse_reits.csv")):
                path = Path("ini") / filename
                try:
                    records.extend(_local_directory(path, "SZSE", "fund"))
                    local_szse_sources.add(source)
                except FileNotFoundError:
                    pass
                except (OSError, ValueError, csv.Error) as error:
                    print(f"SZSE fund directory {path} failed: {type(error).__name__}: {error}", file=sys.stderr)
            inactive_bonds_path = Path("ini") / "inactive_convertible_bonds.txt"
            inactive_bonds = set()
            try:
                entries = inactive_bonds_path.read_text(encoding="utf-8-sig").splitlines()
                codes = {entry.partition("#")[0].strip() for entry in entries}
                codes.discard("")
                if any(not re.fullmatch(r"[0-9]{6}\.(SSE|SZSE|BSE)", code) for code in codes):
                    raise ValueError("expected a six-digit convertible-bond code and exchange")
                inactive_bonds = codes
            except FileNotFoundError:
                pass
            except (OSError, ValueError) as error:
                print(f"Inactive convertible bonds {inactive_bonds_path} failed: {type(error).__name__}: {error}", file=sys.stderr)
            for name in ("fund_etf_spot_em", "fund_lof_spot_em", "bond_zh_hs_cov_spot"):
                if name in official_sse_sources and name in local_szse_sources:
                    continue
                try:
                    frame = getattr(ak, name)()
                    for row in frame.to_dict(orient="records"):
                        if name == "bond_zh_hs_cov_spot":
                            symbol = str(row["symbol"]).lower()
                            exchange = {"sh": "SSE", "sz": "SZSE"}.get(symbol[:2])
                            code, label, kind = symbol[2:], str(row["name"]), "convertible_bond"
                            if f"{code}.{exchange}" in inactive_bonds:
                                continue
                        else:
                            code, label, kind = str(row["代码"]), str(row["名称"]), "fund"
                            exchange = {"5": "SSE", "1": "SZSE"}.get(code[:1])
                        if exchange == "SSE" and name in official_sse_sources:
                            continue
                        if exchange == "SZSE" and name in local_szse_sources:
                            continue
                        if exchange and len(code) == 6 and code.isascii() and code.isdigit():
                            records.append({"symbol": code, "name": label, "exchange": exchange, "kind": kind})
                except Exception as error:
                    print(f"akshare {name} failed: {type(error).__name__}: {error}", file=sys.stderr)
        for item in records:
            full_aliases, short_aliases = make_pinyin_aliases(str(item.get("name", "")))
            item["pinyin_full_aliases"] = full_aliases
            item["pinyin_short_aliases"] = short_aliases
        return records

    def daily_bars(self, symbol: str, start: str, end: str, adjustment: str = "", exchange: str = "") -> list[dict]:
        is_index = (exchange == "SSE" and symbol.startswith("000")) or (exchange == "SZSE" and symbol.startswith("399"))
        if exchange == "HKEX" or is_index:
            try:
                import akshare as ak
                frame = ak.stock_hk_daily(symbol=symbol) if exchange == "HKEX" else ak.stock_zh_index_daily(symbol=("sh" if exchange == "SSE" else "sz") + symbol)
                if frame is not None and not frame.empty:
                    frame = frame.rename(columns={"date": "date", "open": "open", "high": "high", "low": "low", "close": "close", "volume": "volume"})
                    frame["date"] = frame["date"].astype(str)
                    frame = frame[(frame["date"] >= start[:4] + "-" + start[4:6] + "-" + start[6:]) &
                                  (frame["date"] <= end[:4] + "-" + end[4:6] + "-" + end[6:])]
                    return frame.to_dict(orient="records")
            except Exception as error:
                source = "stock_hk_daily" if exchange == "HKEX" else "stock_zh_index_daily"
                print(f"akshare {source} failed: {type(error).__name__}: {error}; using Tencent daily history", file=sys.stderr)
            return [] if adjustment else _tencent_daily_bars(symbol, start, end, exchange)
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
        except Exception as error:
            print(f"akshare stock_zh_a_hist failed: {type(error).__name__}: {error}; using Tencent daily history", file=sys.stderr)
        return [] if adjustment else _tencent_daily_bars(symbol, start, end, exchange)

    def minute_bars(self, symbol: str, start: str, end: str, adjustment: str = "", exchange: str = "") -> list[dict]:
        is_index = (exchange == "SSE" and symbol.startswith("000")) or (exchange == "SZSE" and symbol.startswith("399"))
        if exchange == "HKEX" or is_index:
            try:
                import akshare as ak
                frame = (ak.stock_hk_hist_min_em(symbol=symbol, period="1", start_date=start, end_date=end)
                         if exchange == "HKEX" else ak.index_zh_a_hist_min_em(symbol=symbol, period="1", start_date=start, end_date=end))
                if frame is not None and not frame.empty:
                    frame = frame.rename(columns={
                        "时间": "date", "开盘": "open", "最高": "high", "最低": "low", "收盘": "close",
                        "成交量": "volume", "成交额": "turnover",
                    })
                    frame["date"] = frame["date"].astype(str)
                    return frame.to_dict(orient="records")
            except Exception as error:
                source = "stock_hk_hist_min_em" if exchange == "HKEX" else "index_zh_a_hist_min_em"
                print(f"akshare {source} failed: {type(error).__name__}: {error}", file=sys.stderr)
                raise
            return []
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
        except Exception as error:
            print(f"akshare stock_zh_a_hist_min_em failed: {type(error).__name__}: {error}", file=sys.stderr)
            raise
        return []

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
