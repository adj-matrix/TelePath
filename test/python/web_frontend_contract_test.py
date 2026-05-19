#!/usr/bin/env python3

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def read_text(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def read_json(relative: str) -> dict:
    return json.loads(read_text(relative))


def assert_contains(text: str, needle: str) -> None:
    assert needle in text, f"missing expected frontend contract: {needle}"


def test_block_map_snapshot_selector_contract() -> None:
    index_html = read_text("web/frontend/index.html")
    state_js = read_text("web/frontend/js/core/state.js")
    block_map_js = read_text("web/frontend/js/ui/block-map.js")
    components_css = read_text("web/frontend/css/components.css")

    assert_contains(index_html, 'id="blockMapSnapshotSelect"')
    assert_contains(index_html, 'data-i18n="block_map.controls.snapshot"')
    assert_contains(state_js, "blockMapSnapshotSelect")
    assert_contains(state_js, "selectedSnapshotKey")
    assert_contains(block_map_js, "buildSnapshotChoices")
    assert_contains(block_map_js, "sampled_snapshots")
    assert_contains(block_map_js, "selectedSnapshotKey")
    assert_contains(block_map_js, 'addEventListener("change"')
    assert_contains(components_css, ".blockmap-controls")


def test_block_map_selector_localization_contract() -> None:
    for locale_path in (
        "localization/web-console/en.json",
        "localization/web-console/zh-CN.json",
    ):
        payload = read_json(locale_path)
        block_map = payload["block_map"]
        controls = block_map["controls"]
        reasons = block_map["reasons"]
        assert controls["snapshot"]
        assert controls["final_snapshot"]
        assert "{reason}" in controls["sample_template"]
        assert reasons["after_read"]
        assert reasons["after_mark_dirty"]
        assert reasons["runtime_io"]


def main() -> None:
    test_block_map_snapshot_selector_contract()
    test_block_map_selector_localization_contract()


if __name__ == "__main__":
    main()
