require "fileinto";
if address :domain :is :comparator "i;ascii-casemap" "From" "example.org" {
  fileinto "apex";
  stop;
}
fileinto "unsorted";
