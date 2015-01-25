# file: jaromail.bash
# jaro parameter-completion for Bash

# location hardcoded is Mail
jarodir=${HOME}/Mail


_jaro() {  #+ starts with an underscore.
  local cur
  # Pointer to current completion word.

  COMPREPLY=()   # Array variable storing the possible completions.
#  cur=${COMP_WORDS[COMP_CWORD]}
  curlen=$(( ${#COMP_WORDS[@]} - 1 ))
  cur=${COMP_WORDS[$curlen]}

  case "$cur" in
      *)
	  COMPREPLY=( $( compgen -W 'fetch send peek compose open' -- $cur ) )
	  ;;

      open)
	  for f in `ls $jarodir`; do
	      COMPREPLY+=( $( compgen -W "$f" -- $cur ) )
	  done
	  ;;

#   Generate the completion matches and load them into $COMPREPLY array.
#   xx) May add more cases here.
#   yy)
#   zz)
  esac

echo 
echo "words: cur[$cur] len[$curlen] word[${COMP_WORDS[$curlen]}]"
echo "current array: ${COMP_WORDS[@]}"

  return 0
}

complete -F _jaro -o filenames jaro

