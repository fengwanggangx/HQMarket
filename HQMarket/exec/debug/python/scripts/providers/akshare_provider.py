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

    def industry_sectors(self) -> list[dict]:
        import akshare as ak
        try:
            frame = ak.stock_board_industry_name_em()
            frame = frame.rename(columns={
                "板块代码": "code", "板块名称": "name", "涨跌幅": "change_percent",
                "上涨家数": "rising_count", "下跌家数": "falling_count",
                "领涨股票": "leading_name",
            })
        except Exception:
            try:
                frame = ak.index_realtime_sw(symbol="一级行业")
                frame = frame.rename(columns={
                    "指数代码": "code", "指数名称": "name", "日涨跌幅": "change_percent",
                })
            except Exception:
                sectors = [
                    ("801010", "农林牧渔"), ("801030", "基础化工"), ("801040", "钢铁"),
                    ("801050", "有色金属"), ("801080", "电子"), ("801110", "家用电器"),
                    ("801120", "食品饮料"), ("801130", "纺织服饰"), ("801140", "轻工制造"),
                    ("801150", "医药生物"), ("801160", "公用事业"), ("801170", "交通运输"),
                    ("801180", "房地产"), ("801200", "商贸零售"), ("801210", "社会服务"),
                    ("801230", "综合"), ("801710", "建筑材料"), ("801720", "建筑装饰"),
                    ("801730", "电力设备"), ("801740", "国防军工"), ("801750", "计算机"),
                    ("801760", "传媒"), ("801770", "通信"), ("801780", "银行"),
                    ("801790", "非银金融"), ("801880", "汽车"), ("801890", "机械设备"),
                    ("801950", "煤炭"), ("801960", "石油石化"), ("801970", "环保"),
                    ("801980", "美容护理"),
                ]
                return [{"code": code, "name": name, "change_percent": 0,
                         "rising_count": 0, "falling_count": 0, "flat_count": 0,
                         "member_count": 0, "leading_name": "", "leading_symbol": ""}
                        for code, name in sectors]
        if frame is None:
            return []
        for column in ["change_percent", "rising_count", "falling_count", "flat_count"]:
            if column not in frame:
                frame[column] = 0
        if "member_count" not in frame:
            frame["member_count"] = frame["rising_count"] + frame["falling_count"]
        frame["leading_name"] = frame.get("leading_name", "")
        frame["leading_symbol"] = ""
        columns = ["code", "name", "change_percent", "rising_count", "falling_count",
                   "flat_count", "member_count", "leading_name", "leading_symbol"]
        return frame.reindex(columns=columns).fillna(0).to_dict(orient="records")

    def industry_constituents(self, sector_name: str) -> list[dict]:
        import akshare as ak
        try:
            frame = ak.stock_board_industry_cons_em(symbol=sector_name)
            frame = frame.rename(columns={"代码": "symbol", "名称": "name"})
        except Exception:
            sector_codes = {
                "农林牧渔": "801010", "基础化工": "801030", "钢铁": "801040",
                "有色金属": "801050", "电子": "801080", "家用电器": "801110",
                "食品饮料": "801120", "纺织服饰": "801130", "轻工制造": "801140",
                "医药生物": "801150", "公用事业": "801160", "交通运输": "801170",
                "房地产": "801180", "商贸零售": "801200", "社会服务": "801210",
                "综合": "801230", "建筑材料": "801710", "建筑装饰": "801720",
                "电力设备": "801730", "国防军工": "801740", "计算机": "801750",
                "传媒": "801760", "通信": "801770", "银行": "801780",
                "非银金融": "801790", "汽车": "801880", "机械设备": "801890",
                "煤炭": "801950", "石油石化": "801960", "环保": "801970",
                "美容护理": "801980",
            }
            sector_code = sector_codes.get(sector_name)
            if sector_code is None:
                return []
            frame = ak.index_component_sw(symbol=sector_code)
            frame = frame.rename(columns={"证券代码": "symbol", "证券名称": "name"})
        if frame is None:
            return []
        return frame.reindex(columns=["symbol", "name"]).fillna("").to_dict(orient="records")
