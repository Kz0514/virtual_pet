"""
LLM 回复解析与清洗 单元测试 (纯标准库, 本地可跑):
    cd server && python -m unittest discover -s tests -v
"""
import sys, os, unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from app.utils.reply_parser import parse_reply, clean_text, FALLBACK_TEXT


class TestParseReply(unittest.TestCase):
    def test_valid_json(self):
        text, meta = parse_reply(
            '{"mood_delta":3,"exp":2,"animation":"happy","text":"[inst:开心]主人好呀"}'
        )
        self.assertEqual(text, "[inst:开心]主人好呀")
        self.assertEqual(meta["mood_delta"], 3)
        self.assertEqual(meta["exp"], 2)
        self.assertEqual(meta["animation"], "happy")

    def test_json_with_trailing_junk(self):
        text, meta = parse_reply('{"mood_delta":-2,"text":"干嘛……"} 多余尾巴')
        self.assertEqual(text, "干嘛……")
        self.assertEqual(meta["mood_delta"], -2)

    def test_first_line_json(self):
        text, meta = parse_reply('{"mood_delta":0,"text":"好的~"}\n第二行内容')
        self.assertEqual(text, "好的~")

    def test_malformed_json_extracts_text(self):
        # 模型偶尔输出残缺 JSON (漏右括号等)
        text, _ = parse_reply('{"mood_delta":3,"text":"你好呀",')
        self.assertEqual(text, "你好呀")

    def test_malformed_json_no_text_uses_fallback(self):
        text, _ = parse_reply('{"mood_delta":3,"animation"')
        self.assertEqual(text, FALLBACK_TEXT)
        self.assertNotIn("{", text)

    def test_json_header_then_plain_text(self):
        text, _ = parse_reply('{"mood_delta":3}\n主人好呀')
        self.assertEqual(text, "主人好呀")

    def test_plain_text_passthrough(self):
        # LLM 调用失败时的兜底文案是纯文本, 应原样通过
        text, _ = parse_reply("萝莉丝睡着了...(timeout)")
        self.assertEqual(text, "萝莉丝睡着了...(timeout)")

    def test_escaped_quotes_in_text(self):
        text, _ = parse_reply('{"mood_delta":0,"text":"他说\\"你好\\""}')
        self.assertEqual(text, '他说"你好"')

    def test_json_without_text_key(self):
        text, _ = parse_reply('{"mood_delta":3}')
        self.assertEqual(text, FALLBACK_TEXT)

    def test_never_leak_json_structure(self):
        """用户实测缺陷: JSON 头漏过滤被 TTS 朗读 — 保证任何畸形 JSON 不外泄"""
        cases = [
            '{"mood_delta":1,"text":"你好"',   # 漏右括号
            '{"mood_delta":1,',                # 更残
            '{"mood_delta":1',                 # 半截
            '{"mood_delta" 你好',              # 乱序
            '{"mood_delta":1}xxx',             # 头部 JSON + 垃圾尾
            '{{"mood_delta":1}}',              # 双花括号
        ]
        for c in cases:
            text, _ = parse_reply(c)
            self.assertNotIn("{", text, f"JSON leaked for {c!r} -> {text!r}")
            self.assertNotIn("mood_delta", text)


class TestCleanText(unittest.TestCase):
    def test_strip_pause_markers(self):
        self.assertEqual(clean_text("嗯...|p500好的~|p300"), "嗯...好的~")

    def test_strip_inst_tag(self):
        self.assertEqual(clean_text("[inst:开心的萝莉音]主人好呀"), "主人好呀")

    def test_strip_tools(self):
        self.assertEqual(clean_text("/tools.weather,state"), "")
        self.assertEqual(clean_text("/tools.place_search(keyword=咖啡厅)"), "")
        self.assertEqual(clean_text("先看看/tools.weather再聊"), "先看看再聊")

    def test_combined(self):
        self.assertEqual(
            clean_text("[inst:温柔]|p300今天/tools.weather,state不错"), "今天不错"
        )


if __name__ == "__main__":
    unittest.main()
