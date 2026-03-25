# tests/test_scheduler.py
from unittest.mock import patch, MagicMock
import subprocess

def test_register_constructs_correct_schtasks_command():
    from src.scheduler import register_boot_task, TASK_NAME
    with patch("src.scheduler.subprocess.run") as mock_run:
        mock_run.return_value = MagicMock(returncode=0)
        result = register_boot_task(
            python_exe=r"C:\Python\pythonw.exe",
            script_path=r"C:\App\boot_apply.py",
        )
        assert result is True
        args = mock_run.call_args_list[-1][0][0]
        assert "schtasks" in args[0].lower() or args[0] == "schtasks"
        assert "/create" in args
        assert TASK_NAME in " ".join(args)

def test_unregister_calls_schtasks_delete():
    from src.scheduler import unregister_boot_task, TASK_NAME
    with patch("src.scheduler.subprocess.run") as mock_run:
        mock_run.return_value = MagicMock(returncode=0)
        result = unregister_boot_task()
        assert result is True
        args = mock_run.call_args[0][0]
        assert "/delete" in args
        assert TASK_NAME in args

def test_is_registered_returns_true_when_task_exists():
    from src.scheduler import is_task_registered
    with patch("src.scheduler.subprocess.run") as mock_run:
        mock_run.return_value = MagicMock(returncode=0)
        assert is_task_registered() is True

def test_is_registered_returns_false_when_missing():
    from src.scheduler import is_task_registered
    with patch("src.scheduler.subprocess.run") as mock_run:
        mock_run.return_value = MagicMock(returncode=1)
        assert is_task_registered() is False
