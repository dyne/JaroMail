require "fileinto";
if address :all :is :comparator "i;ascii-casemap" ["To", "Cc"] "person@example.net" {
  fileinto "exact mailbox";
  stop;
}
fileinto "unsorted";
