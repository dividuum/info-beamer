DOCS = userguide.html installation.html developer.html

%.html: %.md header.html footer.html
	(cat header.html; markdown $<; cat footer.html) > $@

all: $(DOCS)

.PHONY: clean
clean:
	rm -f $(DOCS)

