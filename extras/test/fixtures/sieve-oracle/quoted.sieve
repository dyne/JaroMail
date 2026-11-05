require ["fileinto"];
if address :all :is :comparator "i;ascii-casemap" "From"
    ["\"a b\"@example.org", "\"a\\\"b\"@example.org"] {
  fileinto "quoted";
  stop;
}
fileinto "unsorted";
