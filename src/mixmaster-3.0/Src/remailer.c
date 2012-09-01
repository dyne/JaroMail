/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Simple remailer frontend: Read mix packets from standard input.
   $Id: remailer.c 934 2006-06-24 13:40:39Z rabbi $ */


#include "mix.h"
#include <stdio.h>

/** main *****************************************************************/

/* Returns:
 0 successful operation
 1 error */

int main(int argc, char *argv[])
{
  BUFFER *msg;
  int ret;

  mix_init(NULL);
  msg = buf_new();
  ret = buf_read(msg, stdin);
  if (ret != -1)
    ret = mix_decrypt(msg);

  mix_regular(0);
  mix_exit();
  buf_free(msg);
  return (ret == 0 ? 0 : 1);
}
