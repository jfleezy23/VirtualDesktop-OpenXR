"""Frozen controls entry point, including a bounded local integration check."""
import sys

from vdxr_registry_gui import main


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--self-test":
        from scripts.controls_self_test import run
        raise SystemExit(run(sys.argv[2]))
    if len(sys.argv) != 1:
        raise SystemExit(2)
    main()
