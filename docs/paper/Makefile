# Makefile for Thermal-Core Technical Documentation
# Author: Albert David
# Date: May 2026

# LaTeX compiler
LATEX = pdflatex
BIBTEX = bibtex

# Source and output directories
SRC_DIR = src
OUTPUT_DIR = output

# Main document
MAIN = thermal-core-spec
SRC = $(SRC_DIR)/$(MAIN).tex
PDF = $(OUTPUT_DIR)/$(MAIN).pdf

# Build flags
LATEX_FLAGS = -output-directory=$(OUTPUT_DIR) -halt-on-error

.PHONY: all figures watch clean help

# Default target
all: $(PDF)

# Build PDF (run three times for cross-references, TOC, and stable outlines)
$(PDF): $(SRC) $(wildcard $(SRC_DIR)/*.tex)
	@echo "=== Building Thermal-Core Technical Documentation ==="
	@mkdir -p $(OUTPUT_DIR)
	cd $(SRC_DIR) && $(LATEX) -output-directory=../$(OUTPUT_DIR) -halt-on-error $(MAIN).tex
	cd $(SRC_DIR) && $(LATEX) -output-directory=../$(OUTPUT_DIR) -halt-on-error $(MAIN).tex
	cd $(SRC_DIR) && $(LATEX) -output-directory=../$(OUTPUT_DIR) -halt-on-error $(MAIN).tex
	@echo "=== PDF generated: $(PDF) ==="

# Regenerate the data-driven benchmark figures.  Delegates to the
# root Makefile's `paper-figures` target, which runs the canonical
# scenarios and renders one matplotlib PDF per figure (Stage 16).
figures:
	@echo "=== Regenerating benchmark figures ==="
	$(MAKE) -C ../.. paper-figures

# Iterative writing workflow. Requires latexmk.
watch:
	@mkdir -p $(OUTPUT_DIR)
	cd $(SRC_DIR) && latexmk -pdf -pvc -output-directory=../$(OUTPUT_DIR) $(MAIN).tex

# Clean build artifacts (remove entire output directory)
clean:
	@echo "=== Cleaning build artifacts ==="
	rm -rf $(OUTPUT_DIR)
	@echo "=== Clean complete (output directory removed) ==="

# Help target
help:
	@echo "Thermal-Core Documentation Build System"
	@echo ""
	@echo "Available targets:"
	@echo "  all     - Build PDF (default)"
	@echo "  figures - Regenerate scenario plots + figure manifest"
	@echo "  watch   - Rebuild continuously with latexmk"
	@echo "  clean   - Remove build artifacts"
	@echo "  help    - Show this help message"
	@echo ""
	@echo "Requirements:"
	@echo "  - pdflatex (texlive-latex-base)"
	@echo "  - TikZ (texlive-pictures)"
	@echo "  - Additional packages (texlive-latex-extra)"
	@echo ""
	@echo "Build PDF:"
	@echo "  make figures   # generate plots (rendered PDFs are not"
	@echo "                 # committed); needed once on a fresh clone"
	@echo "  make"
	@echo ""
	@echo "Output: $(PDF)"
