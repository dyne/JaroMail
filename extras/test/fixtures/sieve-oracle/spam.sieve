require ["fileinto"];
if header :is :comparator "i;ascii-casemap" "X-Spam-Flag" "YES" {
  fileinto "Spam";
  stop;
}
fileinto "unsorted";
