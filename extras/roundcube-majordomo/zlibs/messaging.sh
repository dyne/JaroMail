#!/usr/bin/env zsh

init_commandline() {
    ### Options configuration
    #
    # Hi, dear developer!  Are you trying to add a new subcommand, or
    # to add some options?  Well, keep in mind that option names are
    # global: they cannot bear a different meaning or behaviour across
    # subcommands.
    #
    # For example, "-s" means "size" and accepts one argument. If you
    # are tempted to add an alternate option "-s" (e.g., to mean
    # "silent", and that doesn't accept any argument) DON'T DO IT!
    #
    # There are two reasons for that:
    #    I. Usability; users expect that "-s" is "size"
    #   II. Option parsing WILL EXPLODE if you do this kind of bad
    #       things (it will complain: "option defined more than once")
    #
    # If you want to use the same option in multiple commands then you
    # can only use the non-abbreviated long-option version like:
    # -force and NOT -f
    #


    ### Detect subcommand
    local -aU every_opts #every_opts behave like a set; that is, an array with unique elements
    for optspec in $subcommands_opts$main_opts; do
        for opt in ${=optspec}; do
            every_opts+=${opt}
        done
    done
    local -a oldstar
    oldstar=($argv)
    #### detect early: useful for --optiion-parsing
    zparseopts -M -D -Adiscardme ${every_opts}
    if [[ -n ${(k)discardme[--option-parsing]} ]]; then
        echo $1
        if [[ -n "$1" ]]; then
            return 1
        fi
        return 0
    fi
    unset discardme
    if ! zparseopts -M -E -D -Adiscardme ${every_opts}; then
        _failure "Error parsing."
        return 127
    fi
    unset discardme
    subcommand=$1
    if [[ -z $subcommand ]]; then
        subcommand="__default"
    fi

    if [[ -z ${(k)subcommands_opts[$subcommand]} ]]; then
        _warning "There's no such command \"::1 subcommand::\"." $subcommand
        exitv=127 _failure "Please try -h for help."
    fi
    argv=(${oldstar})
    unset oldstar

    ### Parsing global + command-specific options
    # zsh magic: ${=string} will split to multiple arguments when spaces occur
    set -A cmd_opts ${main_opts} ${=subcommands_opts[$subcommand]}
    # if there is no option, we don't need parsing
    if [[ -n $cmd_opts ]]; then
        # here is declared opts=
        zparseopts -M -E -D -Aopts ${cmd_opts}
        if [[ $? != 0 ]]; then
            _warning "Some error occurred during option processing."
            exitv=127 _failure "See \"tomb help\" for more info."
        fi
    fi
    #build PARAM (array of arguments) and check if there are unrecognized options
    ok=0
    PARAM=()
    for arg in $*; do
        if [[ $arg == '--' || $arg == '-' ]]; then
            ok=1
            continue #it shouldnt be appended to PARAM
        elif [[ $arg[1] == '-'  ]]; then
            if [[ $ok == 0 ]]; then
                exitv=127 _failure "Unrecognized option ::1 arg:: for subcommand ::2 subcommand::" $arg $subcommand
            fi
        fi
        PARAM+=$arg
    done
    #first parameter actually is the subcommand: delete it and shift
    if [[ $subcommand != '__default' ]]; then
        PARAM[1]=()
        shift
    fi
    ### End parsing command-specific options
    if ! option_is_set --no-color; then
        autoload colors; colors
    fi

}

# Check an option
option_is_set() {
    # First argument, the commandline flag (i.e. "-s").
    # Return 0 if is set, 1 otherwise

    [[ -n ${(k)opts[$1]} ]];
    return $?;

}

# Get an option value
option_value() {
    # First argument, the commandline flag (i.e. "-s").
    print -n - "${opts[$1]}"
}

# Messaging function with pretty coloring
function _msg() {
    local msg="$(gettext -s "$2")"
    for i in $(seq 3 ${#});
    do
        msg=${(S)msg//::$(($i - 2))*::/$*[$i]}
    done

    local command="print -P"
    local progname="$fg[magenta]${TOMBEXEC##*/}$reset_color"
    local message="$fg_bold[normal]$fg_no_bold[normal]$msg$reset_color"
    local -i returncode

    case "$1" in
        inline)
            command+=" -n"; pchars=" > "; pcolor="yellow"
            ;;
        message)
            pchars=" . "; pcolor="white"; message="$fg_no_bold[$pcolor]$msg$reset_color"
            ;;
        verbose)
            pchars="[D]"; pcolor="blue"
            ;;
        success)
            pchars="(*)"; pcolor="green"; message="$fg_no_bold[$pcolor]$msg$reset_color"
            ;;
        warning)
            pchars="[W]"; pcolor="yellow"; message="$fg_no_bold[$pcolor]$msg$reset_color"
            ;;
        failure)
            pchars="[E]"; pcolor="red"; message="$fg_no_bold[$pcolor]$msg$reset_color"
            returncode=1
            ;;
        print)
            progname=""
            ;;
        *)
            pchars="[F]"; pcolor="red"
            message="Developer oops!  Usage: _msg MESSAGE_TYPE \"MESSAGE_CONTENT\""
            returncode=127
            ;;
    esac
    ${=command} "${progname} $fg_bold[$pcolor]$pchars$reset_color ${message}$color[reset_color]" >&2
    return $returncode
}

function _message say act() {
    local notice="message"
    [[ "$1" = "-n" ]] && shift && notice="inline"
    option_is_set -q || _msg "$notice" $@
    return 0
}

function _verbose xxx func() {
    option_is_set -D || [[ $DEBUG = 1 ]] && _msg verbose $@
    return 0
}

function _success yes notice() {
    option_is_set -q || _msg success $@
    return 0
}

function _warning no  warning() {
    option_is_set -q || _msg warning $@
    return 1
}

function _failure fatal die error() {
    typeset -i exitcode=${exitv:-1}
    option_is_set -q || _msg failure $@
    return 1
}

function _print() {
    option_is_set -q || _msg print $@
    return 0
}
