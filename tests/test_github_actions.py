# CHANGE_SUMMARY
# 2026-07-25  kilo
#   - Added tests for the GitHub Actions orchestration scripts.
#   - Covers run_chunk.py synthetic fallback, report building, aggregate.py
#     combination/chart generation, run_local.py CLI, and workflow YAML shape.
# WHY: Smoke tests must pass even when other agents' modules are not yet merged.

import importlib
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

PROJECT_ROOT = Path(__file__).resolve().parent.parent
SCRIPTS_DIR = PROJECT_ROOT / "scripts"


def _import_run_chunk(monkeypatch=None):
    """Import run_chunk.py with the test fallback enabled."""
    if monkeypatch is not None:
        monkeypatch.setenv("TOPSTEP_GH_ACTIONS_TEST", "1")
    else:
        os.environ["TOPSTEP_GH_ACTIONS_TEST"] = "1"
    sys.path.insert(0, str(SCRIPTS_DIR))
    if "run_chunk" in sys.modules:
        del sys.modules["run_chunk"]
    return importlib.import_module("run_chunk")


def test_run_chunk_synthetic_fallback(tmp_path, monkeypatch):
    run_chunk = _import_run_chunk(monkeypatch)

    out_file = tmp_path / "result.json"
    data_file = tmp_path / "NQ_1min.csv"
    data_file.write_text("timestamp,open,high,low,close,volume\n")

    run_chunk.main([
        "--strategy", "kasen_orb",
        "--start-date", "2024-01-01",
        "--end-date", "2024-01-31",
        "--output", str(out_file),
        "--data-path", str(data_file),
    ])

    assert out_file.exists()
    report = json.loads(out_file.read_text())
    assert report["strategy"] == "kasen_orb"
    assert report["start_date"] == "2024-01-01"
    assert report["end_date"] == "2024-01-31"
    assert "raw" in report
    assert "topstep" in report
    for mode in ("raw", "topstep"):
        for key in [
            "trades", "winning_trades", "losing_trades", "win_rate", "avg_trade",
            "profit_factor", "gross_profit", "gross_loss", "max_drawdown",
            "sharpe", "cagr", "final_equity", "metrics_keys",
        ]:
            assert key in report[mode]


def test_run_chunk_build_report():
    run_chunk = _import_run_chunk()

    backtest_result = {
        "summary": {
            "total_trades": 10,
            "winning_trades": 6,
            "losing_trades": 4,
            "win_rate": 0.6,
            "avg_trade": 5.0,
            "profit_factor": 1.5,
            "gross_profit": 300.0,
            "gross_loss": -200.0,
            "max_drawdown": -100.0,
            "sharpe": 1.2,
            "cagr": 0.1,
        },
        "equity_curve": list(range(11)),
    }
    metrics = {"psr": 0.8, "dsr": 0.7, "sharpe": 1.2, "max_drawdown": -100.0}

    report = run_chunk._build_report(
        "nitro_crt", "2024-01-01", "2024-01-31", backtest_result, metrics, {}
    )

    assert report["strategy"] == "nitro_crt"
    assert report["trades"] == 10
    assert report["winning_trades"] == 6
    assert report["losing_trades"] == 4
    assert report["win_rate"] == pytest.approx(0.6)
    assert report["avg_trade"] == pytest.approx(5.0)
    assert report["profit_factor"] == pytest.approx(1.5)
    assert report["gross_profit"] == pytest.approx(300.0)
    assert report["gross_loss"] == pytest.approx(-200.0)
    assert report["max_drawdown"] == pytest.approx(-100.0)
    assert report["sharpe"] == pytest.approx(1.2)
    assert report["cagr"] == pytest.approx(0.1)
    assert report["final_equity"] == pytest.approx(10.0)
    assert "metrics_keys" in report


def _make_agg_record(strategy, start, end, trades, win_rate, avg_trade, gross_profit, gross_loss, final_equity):
    rec = {
        "trades": trades,
        "executed_trades": trades,
        "skipped_trades": 0,
        "winning_trades": int(trades * win_rate),
        "losing_trades": trades - int(trades * win_rate),
        "win_rate": win_rate,
        "avg_trade": avg_trade,
        "profit_factor": gross_profit / abs(gross_loss) if gross_loss else 0.0,
        "gross_profit": gross_profit,
        "gross_loss": gross_loss,
        "max_drawdown": -0.05,
        "max_drawdown_dollar": -1000.0,
        "max_sod_drawdown_dollar": -500.0,
        "avg_sod_drawdown_dollar": -200.0,
        "sharpe": 0.8,
        "cagr": 0.05,
        "total_return": (final_equity - 100_000.0) / 100_000.0,
        "start_equity": 100_000.0,
        "final_equity": final_equity,
        "metrics_keys": ["test"],
        "topstep_enabled": False,
        "daily_limit_hits": 0,
        "trailing_limit_hits": 0,
        "account_failed": False,
        "profit_target_reached": False,
    }
    return {
        "strategy": strategy,
        "start_date": start,
        "end_date": end,
        "raw": rec.copy(),
        "topstep": rec.copy(),
    }


def test_aggregate_combines_chunks_and_creates_chart(tmp_path):
    chunks_dir = tmp_path / "chunks"
    chunks_dir.mkdir()

    records = [
        _make_agg_record("kasen_orb", "2024-01-01", "2024-01-31", 100, 0.55, 2.0, 200.0, -180.0, 100_020.0),
        _make_agg_record("nitro_crt", "2024-02-01", "2024-02-29", 80, 0.45, 1.5, 150.0, -140.0, 100_010.0),
        _make_agg_record("kasen_orb", "2024-03-01", "2024-03-31", 120, 0.5, 1.0, 180.0, -170.0, 100_010.0),
    ]
    for i, rec in enumerate(records):
        (chunks_dir / f"result_{i}.json").write_text(json.dumps(rec))

    output_dir = tmp_path / "final"
    subprocess.run(
        [sys.executable, str(SCRIPTS_DIR / "aggregate.py"), "--input", str(chunks_dir), "--output", str(output_dir)],
        cwd=PROJECT_ROOT,
        check=True,
    )

    report_path = output_dir / "final_report.json"
    assert report_path.exists()

    report = json.loads(report_path.read_text())
    assert "raw" in report
    assert "topstep" in report
    for mode in ("raw", "topstep"):
        assert "overall" in report[mode]
        assert "by_strategy" in report[mode]
        assert "kasen_orb" in report[mode]["by_strategy"]
        assert "nitro_crt" in report[mode]["by_strategy"]
    assert len(report["chunks"]) == 3
    assert (output_dir / "equity_by_strategy_raw.png").exists()
    assert (output_dir / "equity_by_strategy_topstep.png").exists()


def test_run_local_help():
    result = subprocess.run(
        [sys.executable, str(SCRIPTS_DIR / "run_local.py"), "--help"],
        cwd=PROJECT_ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    assert "--strategies" in result.stdout


def test_workflow_yaml_shape():
    wf_path = PROJECT_ROOT / ".github" / "workflows" / "topstep_parallel.yml"
    text = wf_path.read_text()
    assert "kasen_orb" in text
    assert "nitro_crt" in text
    assert text.count("- {start:") == 20
    assert "gh release download" in text
    assert "actions/download-artifact@v4" in text
    assert "actions/upload-artifact@v4" in text
    assert "aggregate.py" in text
