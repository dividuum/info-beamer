DOCS = manual.html

%.html: %.md header.html footer.html
	(cat header.html; markdown_py -x toc -x tables -x codehilite $<; cat footer.html) > $@

all: $(DOCS)

.PHONY: clean
clean:
	rm -f $(DOCS)

