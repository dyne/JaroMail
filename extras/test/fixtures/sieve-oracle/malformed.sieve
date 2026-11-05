require "fileinto";
if address :domain :is "From" "example.org" {
  fileinto "apex";
