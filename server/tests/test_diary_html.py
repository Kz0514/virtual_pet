"""
日记 HTML 渲染 单元测试 (纯函数, 本地可跑):
    cd server && python -m unittest discover -s tests -v
"""
import base64
import os
import sys
import tempfile
import unittest
from types import SimpleNamespace
from datetime import date

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from app.services.diary_service import (
    DOODLE_DIR,
    load_doodle_b64,
    render_diary_html,
    _safe_doodle_path,
)


def _entry(**kw):
    base = dict(
        entry_date=date(2026, 8, 20),
        title="摸头的一天",
        content="今天主人摸了我的头。\n好开心呀!",
        mood_summary="开心",
    )
    base.update(kw)
    return SimpleNamespace(**base)


class TestSafeDoodlePath(unittest.TestCase):
    def test_valid_batch_url(self):
        p = _safe_doodle_path("/doodles/batch/grid_3.png")
        self.assertTrue(p)
        self.assertTrue(p.startswith(os.path.realpath(DOODLE_DIR) + os.sep))

    def test_non_doodles_prefix_rejected(self):
        self.assertIsNone(_safe_doodle_path("/uploads/evil.png"))
        self.assertIsNone(_safe_doodle_path("//doodles/../etc/passwd"))

    def test_path_traversal_rejected(self):
        self.assertIsNone(_safe_doodle_path("/doodles/../../etc/passwd"))
        self.assertIsNone(_safe_doodle_path("/doodles/%2e%2e/%2e%2e/etc/passwd"))


class TestLoadDoodleB64(unittest.TestCase):
    def test_missing_returns_none(self):
        self.assertIsNone(load_doodle_b64(None))
        self.assertIsNone(load_doodle_b64(""))
        self.assertIsNone(load_doodle_b64("/doodles/batch/does_not_exist.png"))

    def test_traversal_returns_none(self):
        self.assertIsNone(load_doodle_b64("/doodles/../../etc/hostname"))

    def test_existing_file_encoded(self):
        with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as f:
            f.write(b"\x89PNG-fake")
            fname = f.name
        try:
            os.makedirs(os.path.join(DOODLE_DIR, "batch"), exist_ok=True)
            import shutil
            shutil.copy(fname, os.path.join(DOODLE_DIR, "batch", "grid_test.png"))
            uri = load_doodle_b64("/doodles/batch/grid_test.png")
            self.assertTrue(uri.startswith("data:image/png;base64,"))
            payload = uri.split(",", 1)[1]
            self.assertEqual(base64.b64decode(payload), b"\x89PNG-fake")
        finally:
            os.remove(fname)
            os.remove(os.path.join(DOODLE_DIR, "batch", "grid_test.png"))


class TestRenderDiaryHtml(unittest.TestCase):
    def test_basic_structure(self):
        html = render_diary_html(_entry(), None)
        self.assertIn("<html", html)
        self.assertIn("摸头的一天", html)
        self.assertIn("2026-08-20", html)
        self.assertIn("今天主人摸了我的头。", html)
        self.assertIn("好开心呀!", html)
        self.assertIn("☀ 开心", html)          # mood 徽章
        self.assertNotIn("<img", html)          # 无涂鸦时不出现

    def test_xss_escaped(self):
        evil = _entry(title='<script>alert(1)</script>',
                      content='<img src=x onerror=alert(2)>正文',
                      mood_summary='</div><script>')
        html = render_diary_html(evil, None)
        self.assertNotIn("<script>", html)
        self.assertNotIn("<img src=x", html)
        self.assertIn("&lt;script&gt;", html)
        self.assertIn("&lt;img src=x", html)

    def test_blank_title_fallback(self):
        html = render_diary_html(_entry(title="", mood_summary=""), None)
        self.assertIn("2026-08-20 的一天", html)

    def test_empty_content_fallback(self):
        html = render_diary_html(_entry(content="   \n  ", mood_summary=""), None)
        self.assertIn("今天没有留下文字。", html)

    def test_doodle_embedded(self):
        html = render_diary_html(_entry(), "data:image/png;base64,AAAA")
        self.assertIn('<img src="data:image/png;base64,AAAA"', html)

    def test_multiline_content_becomes_paragraphs(self):
        html = render_diary_html(_entry(content="第一行\n第二行\n\n第三行"), None)
        self.assertIn("<p>第一行</p>", html)
        self.assertIn("<p>第二行</p>", html)
        self.assertNotIn("<p>第三行</p>\n<p>", html)  # 空行不产生空段
        self.assertIn("<p>第三行</p>", html)


if __name__ == "__main__":
    unittest.main()
