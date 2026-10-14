# Independent MiSTer Main fork

This is [djjohnson13p's experimental fork](https://github.com/djjohnson13p/Main_MiSTer) of the official MiSTer Main software, developed with AI assistance. It is not an official MiSTer-devel release. Changes, testing status and community test plans are recorded in [fork development status](docs/fork-status.md). Development uses `independent/main`; the current experimental build carries a `-x5` version suffix. No upstream pull requests are planned.

Build with `make`. The inherited `build.sh` also uploads to a device and should only be used after reviewing its deployment behavior.

# Upstream Main_MiSTer information

This repo serves as the home for the MiSTer Main binaries and the Wiki.

For the purposes of getting google to crawl the wiki, here's a link to the (not for humans) [crawlable wiki](https://github-wiki-see.page/m/MiSTer-devel/Wiki_MiSTer/wiki)

If you're a human looking for the wiki, that's [here](https://github.com/MiSTer-devel/Wiki_MiSTer/wiki)

To compile this application, read more about that [here](https://mister-devel.github.io/MkDocs_MiSTer/developer/mistercompile/#general-prerequisites-for-arm-cross-compiling)

This version has been modified to add support for MiSTer Floppy, SCP and IPF support - see https://mister.robsmithdev.co.uk
