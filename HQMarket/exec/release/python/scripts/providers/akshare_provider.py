"""Thin AKShare adapter for instruments and historical daily bars."""
from __future__ import annotations


class AkShareProvider:
    def instruments(self) -> list[dict]:
        import akshare as ak
        frame = ak.stock_info_a_code_name()
        return frame.rename(columns={"code": "symbol", "name": "name"}).to_dict(orient="records")

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
