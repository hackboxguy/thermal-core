"""Render the step-load scenario figure (Stage 16).

Delegates to the shared `_csv` helper -- one script per figure
(PRD section 12.3).  Reads /tmp/scenario-step_load.csv.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import _render  # noqa: E402

if __name__ == "__main__":
    _render.render("step_load",
                   "Step load: closed-loop response to an abrupt "
                   "temperature step")
