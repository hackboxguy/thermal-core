"""Render the heat-soak-ramp scenario figure (Stage 16).

Delegates to the shared `_csv` helper -- one script per figure
(PRD section 12.3).  Reads /tmp/scenario-heat_soak_ramp.csv.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import _render  # noqa: E402

if __name__ == "__main__":
    _render.render("heat_soak_ramp",
                   "Heat-soak ramp: zone temperature and fan response")
