# cmfc

## Introduction

CMFC is a program which builds HTML files from CMF files. CMF, or Custom Markup
Format, is a format I designed in order to make writing webpages for my personal
website easier. It is meant to be simple, easy to write, and readable in both
generated and plaintext forms.

For more information, consult [the manual](https://tirimid.net/tirimid/cmfc.html).

## Dependencies

Software / system dependencies are:

* Make (for build)

## Management

* Run `make` to build CMFC
* Run `make install` as root to install CMFC after build
* Run `make uninstall` as root to remove CMFC from the system

## Usage

After installation:

```
$ cmfc -o file.html file.cmf
```

... will build a HTML file from a CMF file.

## Contributing

Feel free to contribute bugfixes, or to fork the project and start your own one
based on it. I'd rather implement feature additions myself.
