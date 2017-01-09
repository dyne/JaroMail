function zaw-src-jaro-addr() {
    alladdr="$(jaro addr)"
    : ${(A)candidates::=${(f)alladdr}}
    # : ${(A)cand_descriptions::=${(f)alladdr}}
    actions=(zaw-src-jaro-addemail)
    act_descriptions=("append email address")
    # options=(-t "$title")
}

 function zaw-src-jaro-addemail(){
     BUFFER="$BUFFER `print $1 | awk '{ for (i=1;i<=NF;i++)
     if ( $i ~ /[[:alnum:]]@[[:alnum:]]/ ) {
       gsub(/<|>|,/ , "" , $i); print $i } }'`"

 }

 zaw-register-src -n jaro-addr zaw-src-jaro-addr
