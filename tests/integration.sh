#!/bin/sh
# integration.sh - end-to-end tests: run real command lines through the
# eclipse binary and compare output and exit status.
#
#   sh tests/integration.sh [path/to/eclipse]
#
# Everything happens inside a temporary directory with its own HOME, so
# the tests never read or write your real config or history.

ECLIPSE=${1:-./eclipse}
case $ECLIPSE in
    /*) ;;
    *) ECLIPSE=$(pwd)/$ECLIPSE ;;
esac
if [ ! -x "$ECLIPSE" ]; then
    echo "integration tests: '$ECLIPSE' is not an executable (run 'make' first)" >&2
    exit 2
fi

ROOT=$(mktemp -d "${TMPDIR:-/tmp}/eclipse-it-XXXXXX") || exit 2
trap 'rm -rf "$ROOT"' EXIT INT TERM

HOME=$ROOT/home
WORK=$ROOT/work
mkdir -p "$HOME/Documents" "$HOME/Music" "$WORK"
export HOME
unset XDG_CONFIG_HOME XDG_DATA_HOME ECLIPSE_HISTORY OLDPWD NO_COLOR
cd "$WORK" || exit 2

pass=0
fail=0

ok() {
    pass=$((pass + 1))
}

bad() {
    fail=$((fail + 1))
    printf 'FAIL: %s\n' "$1"
    shift
    for line in "$@"; do
        printf '      %s\n' "$line"
    done
}

# check NAME COMMAND EXPECTED_STDOUT [EXPECTED_STATUS]
check() {
    got=$("$ECLIPSE" --norc -c "$2" 2>/dev/null)
    status=$?
    if [ "$got" = "$3" ] && [ "$status" -eq "${4:-0}" ]; then
        ok
    else
        bad "$1" "command:  $2" "expected: [$3] (status ${4:-0})" \
            "got:      [$got] (status $status)"
    fi
}

# check_err NAME COMMAND STDERR_SUBSTRING [EXPECTED_STATUS]
check_err() {
    got=$("$ECLIPSE" --norc -c "$2" 2>&1 >/dev/null)
    status=$?
    case $got in
        *"$3"*) matched=1 ;;
        *) matched=0 ;;
    esac
    if [ "$matched" -eq 1 ] && { [ -z "$4" ] || [ "$status" -eq "$4" ]; }; then
        ok
    else
        bad "$1" "command:  $2" "expected stderr containing: [$3] (status ${4:-any})" \
            "got:      [$got] (status $status)"
    fi
}

# check_stdin NAME SCRIPT EXPECTED_STDOUT [EXPECTED_STATUS]
check_stdin() {
    got=$(printf '%s\n' "$2" | "$ECLIPSE" 2>/dev/null)
    status=$?
    if [ "$got" = "$3" ] && [ "$status" -eq "${4:-0}" ]; then
        ok
    else
        bad "$1" "script:   $2" "expected: [$3] (status ${4:-0})" \
            "got:      [$got] (status $status)"
    fi
}

NL='
'

# ---- running programs ------------------------------------------------
check "external command"          'printf "%s-%s\n" a b'            'a-b'
check "builtin echo"              'echo hello world'                'hello world'
check "echo -n"                   'echo -n no-newline; echo !'      'no-newline!'
check "echo -e"                   'echo -e "a\tb\\\\c"'             "a	b\\c"
check "exit status of false"      'false'                           ''  1
check "exit status of true"       'true'                            ''  0
check "explicit exit code"        'exit 7'                          ''  7
check "exit stops the line"       'exit 3; echo not-reached'        ''  3
check "status variable"           'false; echo $?'                  '1'
check "absolute path"             '/bin/echo abs'                   'abs'
check "PATH lookup"               'PATH=/nonexistent; echo still-builtin' 'still-builtin'
check_err "command not found"     'definitely-not-a-command-xyz'    'command not found' 127
check_err "suggests a correction" 'whereamii'                       'did you mean whereami' 127
check_err "suggestions can be turned off" 'set suggest_commands off; whereamii' 'command not found' 127
check "no suggestion when off"    'set suggest_commands off; whereamii 2>&1 | grep -c "did you mean"' '0' 1
check_err "directory as command"  "$HOME/Documents"                 'is a directory' 126
touch "$WORK/not-executable"
check_err "permission denied"     './not-executable'                'permission denied' 126
check_err "missing file with slash" './missing-program'             'no such file' 127

# ---- quoting and escaping --------------------------------------------
check "single quotes are literal" "echo '\$HOME \"x\" \\n'"         '$HOME "x" \n'
check "double quotes expand"      'X=5; echo "v=$X"'                'v=5'
check "double quotes keep spaces" 'echo "a   b"'                    'a   b'
check "backslash escapes"         'echo a\ \ b \$HOME \"q\"'        'a  b $HOME "q"'
check "adjacent quoted parts"     "echo 'a'\"b\"c"                  'abc'
check "empty string argument"     'printf "[%s]" "" x; echo'        '[][x]'
check "quoted operator is text"   'echo "a | b && c > d"'           'a | b && c > d'
check "comment"                   'echo yes # echo no'              'yes'
check "hash inside a word"        'echo a#b'                        'a#b'
check "newline in quotes"         "echo 'line1${NL}line2'"          "line1${NL}line2"
check "line continuation"         "echo one \\${NL}two"             'one two'
check_err "unclosed quote"        "echo 'oops"                      'unexpected end of input' 2
check_err "syntax error"          'echo a | | b'                    'syntax error' 2
check_err "dangling redirection"  'echo >'                          'expected a file name' 2

# ---- variables and environment ---------------------------------------
check "shell variable"            'A=1; echo $A'                    '1'
check "shell variable is local"   'A=1; printenv A'                 ''  1
check "export"                    'export A=1; printenv A'          '1'
check "export existing"           'A=2; export A; printenv A'       '2'
check "per-command environment"   'A=3 printenv A; echo "[$A]"'     "3${NL}[]"
check "unset"                     'export A=1; unset A; echo "[$A]"' '[]'
check "default value"             'echo ${NOPE:-fallback}'          'fallback'
check "length"                    'A=hello; echo ${#A}'             '5'
check "arithmetic"                'echo $((2 + 3 * 4))'             '14'
check "arithmetic with variables" 'N=6; echo $((N * 7))'            '42'
check "HOME via tilde"            'echo ~'                          "$HOME"
check "tilde in assignment"       'P=~/bin; echo $P'                "$HOME/bin"
check "PATH is honoured"          'PATH=/usr/bin:/bin; type ls | grep -c /'  '1'
check "word splitting"            'A="x y z"; printf "%s\n" $A | wc -l | tr -d " "' '3'
check "no splitting in quotes"    'A="x y z"; printf "%s\n" "$A" | wc -l | tr -d " "' '1'
check_err "bad export name"       'export 1x=3'                     'not a valid variable name' 1
check "version variable"          'echo ${ECLIPSE_VERSION:+set}'    'set'

# positional parameters need extra arguments after the -c command
got=$("$ECLIPSE" --norc -c 'echo $0 $1 $2 $#' name a b 2>/dev/null)
if [ "$got" = "name a b 2" ]; then ok
else bad "positional parameters" "got: [$got]"; fi

# ---- command substitution --------------------------------------------
check "command substitution"      'echo "[$(echo inner)]"'          '[inner]'
check "backticks"                 'echo `echo old-style`'           'old-style'
check "nested substitution"       'echo $(echo $(echo deep))'       'deep'
check "substitution status"       'X=$(exit 4); echo $?'            '4'
check "substitution strips newlines" 'X=$(printf "a\n\n\n"); echo "[$X]"' '[a]'
check "substitution splits"       'printf "%s\n" $(echo a b) | wc -l | tr -d " "' '2'
check "substitution in quotes"    'printf "%s\n" "$(echo a b)" | wc -l | tr -d " "' '1'

# ---- pipes -----------------------------------------------------------
check "simple pipe"               'echo hello | tr a-z A-Z'         'HELLO'
check "three-stage pipe"          'printf "b\na\nc\n" | sort | head -n 1' 'a'
check "pipe status is the last"   'false | true; echo $?'           '0'
check "pipe status failure"       'true | false; echo $?'           '1'
check "builtin in a pipe"         'echo piped | cat'                'piped'
check "pipe across lines"         "echo multi |${NL}cat"            'multi'
check "large pipe does not hang"  'seq 1 100000 | wc -l | tr -d " "' '100000'
check "early reader exit"         'seq 1 100000 | head -n 1'        '1'
check "negation"                  '! false; echo $?'                '0'
check "negated pipeline"          '! true | true; echo $?'          '1'

# ---- redirection -----------------------------------------------------
check "output to file"            'echo data > f1; cat f1'          'data'
check "append"                    'echo a > f2; echo b >> f2; cat f2' "a${NL}b"
check "input from file"           'echo in > f3; tr a-z A-Z < f3'   'IN'
check "stderr to file"            'ls /nonexistent-dir 2> f4; test -s f4 && echo captured' 'captured'
check "stderr to stdout"          'ls /nonexistent-dir 2>&1 | wc -l | tr -d " "' '1'
check "both to file"              'ls /nonexistent-dir . &> f5; test -s f5 && echo both' 'both'
check "redirect a builtin"        'echo quiet > f6; echo loud; cat f6' "loud${NL}quiet"
check "builtin redirect is undone" 'pwd > /dev/null; echo visible'  'visible'
check "here-string"               'cat <<< "from a string"'         'from a string'
check "close stdout"              'echo gone >&-; echo back'        'back'
check "redirect before command"   '> f7 echo early; cat f7'         'early'
check "bare redirection creates"  '> f8; test -f f8 && echo made'   'made'
check "noclobber-free overwrite"  'echo 1 > f9; echo 2 > f9; cat f9' '2'
check "descriptor number"         'sh -c "echo to-three >&3" 3> f10; cat f10' 'to-three'
check_err "missing input file"    'cat < /nonexistent-file'         'cannot open' 1
check_err "unwritable output"     'echo x > /nonexistent-dir/file'  'cannot open' 1
check_err "here-document rejected" 'cat << EOF'                     'here-documents' 2

# ---- chaining --------------------------------------------------------
check "semicolons"                'echo a; echo b'                  "a${NL}b"
check "and: success"              'true && echo yes'                'yes'
check "and: failure"              'false && echo yes'               ''  1
check "or: failure"               'false || echo recovered'         'recovered'
check "or: success"               'true || echo no'                 ''
check "and-or chain"              'false && echo a || echo b'       'b'
check "or-and chain"              'true || echo a && echo b'        'b'
check "status after chain"        'false || false; echo $?'         '1'

# ---- background jobs -------------------------------------------------
check "background and wait"       'sleep 0.1 & wait; echo done'     'done'
check "background does not block" 'sleep 5 & echo immediate; kill $!' 'immediate'
check "bang variable"             'sleep 0.1 & test -n "$!" && echo has-pid; wait' 'has-pid'
check "background list"           'true && echo bg-list & wait'     'bg-list'
check "jobs lists a job"          'sleep 5 & jobs | grep -c "sleep 5"; kill %1' '1'
check "kill by job spec"          'sleep 5 & kill %1; wait; echo killed' 'killed'
check_err "fg needs a terminal"   'fg'                              'interactive' 1

# ---- globbing and braces ---------------------------------------------
mkdir -p "$WORK/g" && touch "$WORK/g/a.txt" "$WORK/g/b.txt" "$WORK/g/c.md"
check "star glob"                 'cd g; echo *.txt'                'a.txt b.txt'
check "question glob"             'cd g; echo ?.md'                 'c.md'
check "quoted glob is literal"    'cd g; echo "*.txt"'              '*.txt'
check "unmatched glob stays"      'cd g; echo *.zip'                '*.zip'
check "brace list"                'echo x{1,2,3}'                   'x1 x2 x3'
check "brace range"               'echo {1..5}'                     '1 2 3 4 5'
check "brace with glob"           'cd g; echo {a,c}.*'              'a.txt c.md'

# ---- builtins --------------------------------------------------------
check "cd and pwd"                "cd $HOME/Documents; pwd"         "$HOME/Documents"
check "cd without argument"       'cd; pwd'                         "$HOME"
check "cd dash"                   "cd $HOME; cd $WORK; cd - >/dev/null; pwd" "$HOME"
check "cd updates PWD"            "cd $HOME; echo \$PWD"            "$HOME"
check "cd updates OLDPWD"         "cd $HOME; echo \$OLDPWD"         "$WORK"
check_err "cd to nowhere"         'cd /no/such/dir'                 'no such directory' 1
check_err "cd suggests"           "cd $HOME/Documnets"              "did you mean '$HOME/Documents'" 1
check_err "cd to a file"          'cd f1'                           'not a directory' 1
check "alias"                     "alias hi='echo hello'${NL}hi there" 'hello there'
check "alias listing"             "alias b='two' a='one'; alias"    "alias a='one'${NL}alias b='two'"
check "alias with a pipe"         "alias up2='tr a-z A-Z'${NL}echo x | up2" 'X'
check "unalias"                   "alias hi='echo hello'${NL}unalias hi${NL}hi 2>/dev/null; echo \$?" '127'
check "alias is next-line only"   "alias hi='echo hello'; hi 2>/dev/null; echo \$?" '127'
check "type: builtin"             'type cd'                         'cd is a shell builtin'
check "type: alias"               "alias ll='ls -l'; type ll"       "ll is an alias for 'ls -l'"
check "type: program"             'type sh | cut -d" " -f1-2'       'sh is'
check_err "type: unknown"         'type no-such-thing-xyz'          'not found' 1
check "source"                    "echo 'SRC=sourced' > lib; source lib; echo \$SRC" 'sourced'
check "source with arguments"     "echo 'echo \$1-\$2' > lib2; . ./lib2 x y" 'x-y'
check_err "source missing file"   'source /no/such/file'            'cannot open' 1
check "set and get a setting"     'set symbol ">>"; set symbol'     '>>'
check "set with equals"           'set autosuggest=off; set autosuggest' 'off'
check_err "set rejects nonsense"  'set color.path sparkly'          'not a colour' 1
check_err "set suggests a key"    'set autosugest off'              "did you mean 'autosuggest'" 1
check_err "set -e is explained"   'set -e'                          'not supported' 2
check "help mentions commands"    'help | grep -c findfile'         '1'
check "help for one command"      'help mkcd | head -n 1'           'mkcd FOLDER - create a directory and enter it'
check "exec replaces the shell"   'exec echo replaced; echo not-reached' 'replaced'
check "history is quiet in -c"    'history | wc -l | tr -d " "'     '0'
check "colon"                     ': ignored; echo $?'              '0'

# ---- human-friendly commands -----------------------------------------
check "whereami"                  'whereami'                        "$WORK"
check "go"                        "go $HOME/Music; pwd"             "$HOME/Music"
check "go without argument"       'go; pwd'                         "$HOME"
check "go falls back to home"     'go Documents >/dev/null; pwd'    "$HOME/Documents"
check "go ignores case"           'go documents >/dev/null; pwd'    "$HOME/Documents"
check "go reports a guess"        'go music'                        '~/Music'
mkdir -p "$WORK/My Folder"
check "go joins words"            'go My Folder; pwd'               "$WORK/My Folder"
check_err "go to nowhere"         'go nowhere-at-all'               'no such directory' 1
check "up"                        "cd $HOME/Documents; up; pwd"     "$HOME"
check "up several"                'mkdir -p d1/d2/d3; cd d1/d2/d3; up 3; pwd' "$WORK"
check_err "up rejects words"      'up there'                        'not a number of levels' 2
check "back"                      "cd $HOME; cd $WORK; back >/dev/null; pwd" "$HOME"
check "back walks the trail"      "cd $HOME; cd Documents; cd ../Music; back >/dev/null; back >/dev/null; pwd" "$HOME"
check "back prints the place"     "cd $HOME/Music; cd $WORK; back"  '~/Music'
check_err "back with no trail"    'back'                            'no earlier directory' 1
check "mkcd"                      'mkcd new/nested/dir; pwd'        "$WORK/new/nested/dir"
check "mkcd into existing"        'mkcd g; pwd'                     "$WORK/g"
check_err "mkcd needs a name"     'mkcd'                            'exactly one folder name' 2
mkdir -p "$WORK/search/sub/.git/objects" && touch "$WORK/search/Report.PDF" \
    "$WORK/search/sub/report-final.txt" "$WORK/search/sub/notes.md" \
    "$WORK/search/sub/.git/objects/report-hidden"
check "findfile by substring"     'cd search; findfile report | sort' "Report.PDF${NL}sub/report-final.txt"
check "findfile with wildcard"    'cd search; findfile "*.md"'      'sub/notes.md'
check "findfile in a folder"      'findfile notes search'           'search/sub/notes.md'
check "findfile marks folders"    'findfile sub search'             'search/sub/'
check_err "findfile no match"     'findfile zzz-nothing search'     'nothing matching' 1
check_err "findfile needs a name" 'findfile'                        'expected a name' 2
check "sysinfo shows the kernel"  'sysinfo | grep -c "Kernel"'      '1'
check "sysinfo shows the shell"   'sysinfo | grep -c "Eclipse"'     '1'
check "recent with no history"    'recent'                          'No commands in history yet.'
check_err "recent rejects words"  'recent lots'                     'how many' 2

# extract: build real archives, then unpack them in a clean directory
mkdir -p "$WORK/arc/src" && echo payload > "$WORK/arc/src/file.txt"
(cd "$WORK/arc" && tar -czf a.tar.gz src && tar -cf b.tar src && \
    cp src/file.txt single.txt && gzip -k single.txt) 2>/dev/null
check "extract tar.gz"            'mkdir x1; cd x1; extract ../arc/a.tar.gz >/dev/null; cat src/file.txt' 'payload'
check "extract tar"               'mkdir x2; cd x2; extract ../arc/b.tar >/dev/null; cat src/file.txt' 'payload'
check "extract gz keeps original" 'cd arc; rm single.txt; extract single.txt.gz >/dev/null; cat single.txt; test -f single.txt.gz && echo kept' "payload${NL}kept"
check "extract reports success"   'mkdir x3; cd x3; extract ../arc/b.tar' 'Extracted ../arc/b.tar'
if command -v zip >/dev/null 2>&1 && command -v unzip >/dev/null 2>&1; then
    (cd "$WORK/arc" && zip -qr c.zip src)
    check "extract zip"           'mkdir x4; cd x4; extract ../arc/c.zip >/dev/null; cat src/file.txt' 'payload'
fi
cp "$WORK/arc/b.tar" "$WORK/arc/-dash.tar"
check "extract name starting with dash" 'mkdir x5; cd x5; cp ../arc/-dash.tar .; extract -dash.tar >/dev/null; cat src/file.txt' 'payload'
check_err "extract unknown type"  'extract f1'                      "don't know how to unpack" 1
check_err "extract missing file"  'extract nothing.zip'             'cannot read' 1
check_err "extract needs a file"  'extract'                         'expected an archive' 2

# ---- scripts, stdin and configuration --------------------------------
printf 'echo script $1\nX=from-script\necho $X\nexit 5\necho unreachable\n' > "$WORK/script.ecl"
got=$("$ECLIPSE" "$WORK/script.ecl" arg 2>/dev/null); status=$?
if [ "$got" = "script arg${NL}from-script" ] && [ "$status" -eq 5 ]; then ok
else bad "script file" "got: [$got] (status $status)"; fi

got=$("$ECLIPSE" /no/such/script 2>&1); status=$?
if [ "$status" -eq 127 ]; then ok; else bad "missing script" "status $status: $got"; fi

check_stdin "commands on stdin"   'echo from-stdin'                 'from-stdin'
check_stdin "multi-line on stdin" "echo 'two${NL}lines'"            "two${NL}lines"
check_stdin "stdin continues after an error" "echo a | | b${NL}echo survived" 'survived'
check_stdin "program shares stdin" "cat${NL}this line is data"      'this line is data'

mkdir -p "$HOME/.config/eclipse"
printf "alias fromrc='echo config-loaded'\nset symbol '>>'\n" > "$HOME/.config/eclipse/config"
got=$("$ECLIPSE" --config "$HOME/.config/eclipse/config" -c 'fromrc; set symbol' 2>&1)
if [ "$got" = "config-loaded${NL}>>" ]; then ok; else bad "config file" "got: [$got]"; fi

printf 'echo good\necho "broken\n' > "$WORK/badrc"
got=$("$ECLIPSE" --config "$WORK/badrc" -c 'echo after' 2>&1)
case $got in
    *"badrc:2:"*"after"*) ok ;;
    *) bad "config error has a line number" "got: [$got]" ;;
esac

chmod o+w "$WORK/badrc"
got=$("$ECLIPSE" --config "$WORK/badrc" -c 'echo after' 2>&1)
case $got in
    *"writable by everyone"*) ok ;;
    *) bad "world-writable config is refused" "got: [$got]" ;;
esac

got=$("$ECLIPSE" --version)
case $got in
    "eclipse "[0-9]*) ok ;;
    *) bad "--version" "got: [$got]" ;;
esac
"$ECLIPSE" --help >/dev/null 2>&1 && ok || bad "--help"
"$ECLIPSE" --bogus >/dev/null 2>&1; [ $? -eq 2 ] && ok || bad "unknown option"

# nothing above may have touched a history file
if [ -e "$HOME/.local/share/eclipse/history" ]; then
    bad "non-interactive runs must not write history"
else
    ok
fi

echo "integration tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
