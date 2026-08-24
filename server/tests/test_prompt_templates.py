"""
提示词名字 token 替换 单元测试 (纯标准库, 本地可跑):
    cd server && python -m unittest discover -s tests -v
"""
import sys, os, unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from app.utils.prompt_templates import (
    apply_names,
    build_system_prompt,
    SYSTEM_PROMPT,
    MEMORY_SUMMARY_PROMPT,
    DIARY_GENERATION_PROMPT,
    TOKEN_PET_NAME,
    TOKEN_OWNER_NAME,
    DEFAULT_PET_NAME,
    DEFAULT_OWNER_NAME,
)


class TestApplyNames(unittest.TestCase):
    def test_default_names(self):
        out = apply_names("你是" + TOKEN_PET_NAME + "，称呼为" + TOKEN_OWNER_NAME, None, None)
        self.assertEqual(out, f"你是{DEFAULT_PET_NAME}，称呼为{DEFAULT_OWNER_NAME}")
        self.assertNotIn("@@", out)

    def test_custom_names(self):
        out = apply_names("你是" + TOKEN_PET_NAME + "，称呼为" + TOKEN_OWNER_NAME,
                          "小咪", "爸爸")
        self.assertEqual(out, "你是小咪，称呼为爸爸")
        self.assertNotIn("@@", out)

    def test_no_tokens_untouched(self):
        out = apply_names("普通文本", None, None)
        self.assertEqual(out, "普通文本")


class TestSystemPrompt(unittest.TestCase):
    def test_default_build_no_residual_tokens(self):
        p = build_system_prompt()
        self.assertNotIn("@@", p)
        self.assertIn(DEFAULT_PET_NAME, p)    # "你是"萝莉丝""
        self.assertIn(DEFAULT_OWNER_NAME, p)  # 称呼默认"主人"

    def test_custom_names_replaced(self):
        p = build_system_prompt(pet_name="小咪", owner_name="爸爸")
        self.assertNotIn("@@", p)
        self.assertNotIn("萝莉丝", p)
        self.assertIn('你是"小咪"', p)
        self.assertIn('称呼用户为"爸爸"', p)

    def test_tool_list_injected(self):
        p = build_system_prompt()
        self.assertIn("/tools.state", p)      # tool_list_text 注入
        self.assertIn("可用工具", p)

    def test_extra_context_appended(self):
        p = build_system_prompt(extra_context="心情: 开心")
        self.assertIn("[当前状态]\n心情: 开心", p)


class TestOtherTemplates(unittest.TestCase):
    def test_memory_summary_tokens_replaced(self):
        out = apply_names(MEMORY_SUMMARY_PROMPT, "小咪", "爸爸")
        self.assertNotIn("@@", out)
        self.assertNotIn("萝莉丝", out)
        self.assertIn('"爸爸:"/"小咪:"行', out)

    def test_diary_prompt_tokens_replaced(self):
        # DIARY_GENERATION_PROMPT 含未转义 JSON 花括号 — .replace 不能崩
        out = apply_names(DIARY_GENERATION_PROMPT, "小咪", "爸爸")
        self.assertNotIn("@@", out)
        self.assertNotIn("萝莉丝", out)
        self.assertIn('你是"小咪"', out)
        self.assertIn("以小咪第一人称书写", out)


if __name__ == "__main__":
    unittest.main()
