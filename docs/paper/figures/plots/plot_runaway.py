"""Render the thermal-runaway scenario figure (Stage 16).

Delegates to the shared `_csv` helper -- one script per figure
(PRD section 12.3).  Reads /tmp/scenario-runaway.csv.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import _render  # noqa: E402

if __name__ == "__main__":
    _render.render("runaway",
                   "Thermal runaway: temperature rising despite "
                   "maximum cooling")
