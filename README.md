# foo_tftags

A foobar2000 component that publishes long titleformat scripts as **global tag fields**. Write a script once (for example your whole BIO_ALBUMARTIST or BIO_ARTIST code) and use `%tf_albumartist%` everywhere that evaluates title formatting: playlist views, CPV, PSS/ELP, JS3 scripts, ESlyrics, Last.fm scribbler, and so on.

## Slots

There are ten slots. Each has a **tag name** and a **titleformat script**. Open **Preferences > Tools > TF Tags**.

| Slot | Default tag name | Default script |
| 01-20 | `01` ... `10` | empty |

The field name is always `tf_` plus the tag name, so slot 1 is `%tf_albumartist%`. Tag names are lowercased and only letters, digits and `_` are kept. Empty or duplicate names get a slot number (`tf_slot4`, `tf_artist_2`). An empty script (or a script that returns nothing) makes the field empty, so `$if(%tf_artist%,...)` works.

## Writing scripts

- Scripts can span several lines. Before compiling, leading and trailing whitespace of every line is removed, lines that start with `//` are dropped, and the lines are joined. Do not rely on line breaks inside quoted text.
- A slot may use other slots: `%tf_artist%` inside slot 3 works. A slot that refers to itself, directly or through other slots, evaluates to nothing instead of looping.
- **Test on selected track** evaluates the script in the editor (saved or not) for the selected playlist item, or the playing track.

## Restart and caching

- foobar2000 reads field names at startup, so **a changed tag name needs a restart**. Script changes apply immediately after **Apply**.
- Results are cached per track and slot for up to 5 seconds, and the cache is dropped when track info or the settings change.


## Includes

- Dynamic (stream) metadata


## Build

Visual Studio 2022, C++17, foobar2000 SDK. Link `foobar2000_SDK`, `foobar2000_component_client` and `pfc`, with the same project settings as foo_countryflags. The file must stay pure ASCII.
