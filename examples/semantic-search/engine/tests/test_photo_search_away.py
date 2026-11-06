import importlib.util
import os
from pathlib import Path
import socket
import sys
import tempfile
import threading
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from photo_search import away  # noqa: E402

NUMPY = importlib.util.find_spec("numpy") is not None


class Hyprland:
    """A Hyprland socket answering `j/monitors all` with `monitors`."""

    def __init__(self, root, monitors):
        folder = Path(root) / "hypr" / "sig"
        folder.mkdir(parents=True)
        self.path = str(folder / ".socket.sock")
        self.listener = socket.socket(socket.AF_UNIX)
        self.listener.bind(self.path)
        self.listener.listen(4)
        self.body = __import__("json").dumps(monitors).encode()
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            try:
                connection, _ = self.listener.accept()
            except OSError:
                return
            with connection:
                connection.recv(100)
                connection.sendall(self.body)


class Away(unittest.TestCase):
    def ask(self, monitors, hyprlock=False):
        with tempfile.TemporaryDirectory() as root:
            server = Hyprland(root, monitors)
            env = {"XDG_RUNTIME_DIR": root, "HYPRLAND_INSTANCE_SIGNATURE": "sig"}
            with mock.patch.dict(os.environ, env), mock.patch.object(away, "locked", return_value=hyprlock):
                result = away.away()
            server.listener.close()
            return result

    def test_a_real_monitor_awake_means_someone_is_there(self):
        self.assertFalse(self.ask([{"make": "Dell", "dpmsStatus": True, "disabled": False}]))

    def test_a_built_in_panel_without_a_make_is_a_real_monitor(self):
        # A MacBook under Asahi: eDP-1 reports an empty make.
        self.assertFalse(self.ask([{"make": "", "name": "eDP-1", "dpmsStatus": True, "disabled": False}]))
        self.assertTrue(self.ask([{"make": "", "name": "eDP-1", "dpmsStatus": False, "disabled": False}]))

    def test_asleep_gone_or_locked_means_away(self):
        self.assertTrue(self.ask([{"make": "Dell", "dpmsStatus": False, "disabled": False}]))
        self.assertTrue(self.ask([{"make": "", "name": "FALLBACK", "dpmsStatus": True, "disabled": False}]))
        self.assertTrue(self.ask([{"make": "", "name": "HEADLESS-2", "dpmsStatus": True, "disabled": False}]))
        self.assertTrue(self.ask([{"make": "Dell", "dpmsStatus": True, "disabled": False}], hyprlock=True))

    def test_without_hyprland_it_cannot_say(self):
        with tempfile.TemporaryDirectory() as root, mock.patch.dict(os.environ, {"XDG_RUNTIME_DIR": root}, clear=False):
            os.environ.pop("HYPRLAND_INSTANCE_SIGNATURE", None)
            self.assertIsNone(away.away())


@unittest.skipUnless(NUMPY, "the search worker requires NumPy")
class Yielding(unittest.TestCase):
    def test_indexing_waits_while_someone_is_there_and_lets_the_model_go_after_a_minute(self):
        from photo_search.worker import Worker

        worker = Worker.__new__(Worker)
        worker.yielding, worker.yielded_at, worker.last_inference = False, None, 1.0
        worker.engine = mock.Mock(idle_seconds=60)
        with mock.patch("photo_search.worker.time.monotonic", return_value=100.0):
            self.assertTrue(worker.yield_to_person(True))
        worker.engine.close.assert_not_called()
        with mock.patch("photo_search.worker.time.monotonic", return_value=161.0):
            self.assertTrue(worker.yield_to_person(True))
        worker.engine.close.assert_called_once()
        self.assertFalse(worker.yield_to_person(False))
        self.assertFalse(worker.yielding)

    def test_a_search_keeps_the_model_a_minute_past_it(self):
        from photo_search.worker import Worker

        worker = Worker.__new__(Worker)
        worker.yielding, worker.yielded_at, worker.last_inference = False, None, None
        worker.engine = mock.Mock(idle_seconds=60)
        with mock.patch("photo_search.worker.time.monotonic", return_value=100.0):
            worker.yield_to_person(True)
        worker.last_inference = 150.0
        with mock.patch("photo_search.worker.time.monotonic", return_value=200.0):
            worker.yield_to_person(True)
        worker.engine.close.assert_not_called()
        with mock.patch("photo_search.worker.time.monotonic", return_value=211.0):
            worker.yield_to_person(True)
        worker.engine.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
