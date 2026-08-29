"""
工具解析契约测试 — 断言 parse_tools/tool_list_text 现状行为 (⑥-6, 为 ⑥-7
拆 tool_registry 提供回归网):
    cd server && python -m unittest discover -s tests -v
"""
import sys, os, unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from app.services.tool_service import parse_tools, tool_list_text, TOOLS


class TestParseTools(unittest.TestCase):
    def test_single_no_param(self):
        self.assertEqual(parse_tools("/tools.weather"),
                         [{"name": "weather", "params": {}}])

    def test_multiple_comma_separated(self):
        # 现状: TOOL_PATTERN 要求 /tools. 前缀, 逗号后第二个 tool 无前缀
        # 不匹配 → 只解析第一个 (docstring 声称支持逗号多 tool, 实际不生效 —
        # 记 backlog, 契约钉现状)
        self.assertEqual(parse_tools("/tools.weather,state"),
                         [{"name": "weather", "params": {}}])

    def test_with_params(self):
        self.assertEqual(
            parse_tools("/tools.geocode(address=北京)"),
            [{"name": "geocode", "params": {"address": "北京"}}])

    def test_mixed_no_param_and_param(self):
        # 同上: 逗号后第二 tool 不解析 (现状)
        self.assertEqual(
            parse_tools("/tools.weather,geocode(address=天安门)"),
            [{"name": "weather", "params": {}}])

    def test_unregistered_tool_filtered(self):
        # 未注册 tool 被静默过滤 (现状行为 — 协议防御)
        self.assertEqual(parse_tools("/tools.not_a_real_tool"), [])
        self.assertEqual(parse_tools("/tools.weather,not_a_real_tool"),
                         [{"name": "weather", "params": {}}])

    def test_empty_and_no_tool_text(self):
        self.assertEqual(parse_tools(""), [])
        self.assertEqual(parse_tools("随便聊聊, 不含工具"), [])

    def test_param_equals_with_spaces_not_parsed(self):
        # 现状: PARAM_PATTERN (\w+)=([^,)]+) 要求 = 紧贴参数名, 两侧空格
        # → 整体不解析 (参数值为空 dict)。契约钉现状
        self.assertEqual(
            parse_tools("/tools.geocode( address =  上海  )"),
            [{"name": "geocode", "params": {}}])
        # 紧凑写法正常解析
        self.assertEqual(
            parse_tools("/tools.geocode(address=上海)"),
            [{"name": "geocode", "params": {"address": "上海"}}])


class TestToolListText(unittest.TestCase):
    def test_every_registered_tool_listed(self):
        lines = tool_list_text().splitlines()
        self.assertEqual(len(lines), len(TOOLS))
        for name, info in TOOLS.items():
            self.assertIn(f"  /tools.{name} — {info['description']}", lines)

    def test_registration_order(self):
        # 现状: 注册顺序 (非排序) — 契约钉现状
        lines = tool_list_text().splitlines()
        self.assertEqual(lines, [f"  /tools.{n} — {TOOLS[n]['description']}"
                                 for n in TOOLS])


if __name__ == "__main__":
    unittest.main()
