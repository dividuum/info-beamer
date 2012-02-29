index.html: index.md header.html footer.html
	(cat header.html; markdown $<; cat footer.html) > $@
	#markdown index.md  > index.html

.PHONY: clean
clean:
	@rm -f index.html

