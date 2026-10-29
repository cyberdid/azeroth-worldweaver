import pathlib
import runpy
import subprocess
import tempfile
import unittest
from unittest import mock


BRIDGE_PATH = pathlib.Path(__file__).resolve().parents[1] / "tools" / "wow-agent" / "codex-bridge.py"
BRIDGE = runpy.run_path(str(BRIDGE_PATH), run_name="codex_bridge_under_test")


class CodexImagePromptTests(unittest.TestCase):
    def test_prompt_is_sent_through_stdin_and_image_argument_is_preserved(self):
        prompt = "Describe this WoW screenshot."
        image_path = "/tmp/wow-frame.png"
        observed = {}

        def fake_codex(command, **_kwargs):
            observed["command"] = command
            observed["input"] = _kwargs["input"]
            output_path = pathlib.Path(command[command.index("-o") + 1])
            output_path.write_text("Visible scene.", encoding="utf-8")
            return subprocess.CompletedProcess(command, 0)

        with tempfile.TemporaryDirectory() as workdir:
            BRIDGE["WORKDIR"] = workdir
            with mock.patch.object(BRIDGE["subprocess"], "run", side_effect=fake_codex):
                reply = BRIDGE["ask_codex"]("gpt-6-luna", "xhigh", prompt, image_path)

        command = observed["command"]
        self.assertEqual(reply, "Visible scene.")
        self.assertEqual(observed["input"], prompt)
        self.assertEqual(command[command.index("-") + 1], "-i")
        self.assertEqual(command[command.index("-i") + 1], image_path)


if __name__ == "__main__":
    unittest.main()
