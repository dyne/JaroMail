require "fileinto";
if address :domain :matches :comparator "i;ascii-casemap" "From" "*.example.org" {
  fileinto "subdomains";
  stop;
}
fileinto "unsorted";
