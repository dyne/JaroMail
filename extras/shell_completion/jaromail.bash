# Bash completion for jaro. Keep every path or ID as one suggestion.
jarodir=${JAROMAILDIR:-${HOME}/Mail}

_jaro() {
  local cur previous word command_index=1 suggestions='' path
  COMPREPLY=()
  previous="${COMP_WORDS[COMP_CWORD-1]}"
  cur="${COMP_WORDS[COMP_CWORD]}"
  while (( command_index < COMP_CWORD )); do
    case "${COMP_WORDS[command_index]}" in
      -a|-l|--account|--list) (( command_index += 2 )) ;;
      -*) (( command_index += 1 )) ;;
      *) break ;;
    esac
  done
  if [[ "${COMP_WORDS[command_index]}" == outbox ]] && (( COMP_CWORD > command_index )); then
    case "$previous" in
      outbox) suggestions='list retry archive' ;;
      retry|archive)
        while IFS= read -r word; do
          [[ "$word" == "$cur"* ]] && COMPREPLY+=("$word")
        done < <(jaro outbox list 2>/dev/null | awk 'NR > 1 { print $1 }') ;;
      *)
        [[ " ${COMP_WORDS[*]} " != *' retry '* ]] || suggestions='--confirm-duplicate' ;;
    esac
  elif [[ "$previous" == open ]]; then
    for path in "$jarodir"/*; do
      [[ -d "$path/cur" && "${path##*/}" == "$cur"* ]] && COMPREPLY+=("${path##*/}")
    done
  else
    suggestions='fetch send outbox queue peek serve compose open update sieve sieve-import filter wizard'
  fi
  if [[ -n "$suggestions" ]]; then
    while IFS= read -r word; do
      COMPREPLY+=("$word")
    done < <(compgen -W "$suggestions" -- "$cur")
  fi
  return 0
}

complete -F _jaro -o filenames jaro
