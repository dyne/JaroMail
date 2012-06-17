/**************************************************************************
 * TagCloud version 1.1.2
 *
 * (c) 2006 Lyo Kato <lyo.kato@gmail.com>
 * TagCloud is freely distributable under the terms of an MIT-style license.
 * 
/**************************************************************************/

var TagCloud = {
  Version: '1.1.2',
  create: function() {
    return new TagCloud.Container();
  }
}

TagCloud.Tag = function (name, count, url, epoch) {
  this.name  = name;
  this.count = count;
  this.url   = url;
  this.epoch = epoch;
  this.style = {};
  this.initClasses();
  this.initAnchorClasses();
}

TagCloud.Tag.prototype.initClasses = function () {
  this.classes = ['tagcloud-base'];
}

TagCloud.Tag.prototype.initAnchorClasses = function () {
  this.anchorClasses = ['tagcloud-anchor'];
}

TagCloud.Tag.prototype.attachClass = function (className) {
  this.classes.push(className);
}

TagCloud.Tag.prototype.attachAnchorClass = function (className) {
  this.anchorClasses.push(className);
}

TagCloud.Tag.prototype.toElement = function() {
  var element = document.createElement('li');
  var linkElement = document.createElement('a');
  linkElement.setAttribute('href', this.url);
  var text = document.createTextNode(this.name);
  linkElement.appendChild(text);
  linkElement.className = this.anchorClasses.join(" ");
  element.appendChild(linkElement);
  element.className = this.classes.join(" ");
  for (var prop in this.style)
    element.style[prop] = this.style[prop];
  return element;
}

TagCloud.Container = function () {
  this.reset();
  this.classes = {
    list: 'tagcloud-list'
  };
}

TagCloud.Container.prototype.reset = function () {
  this.tags      = new Array();
  this.effectors = new Array();
}

TagCloud.Container.prototype.clear = function () {
  this.tags = new Array();
}

TagCloud.Container.prototype.add = function (name, count, url, epoch) {
  if (count == 0)
    throw "TagCloud.Container.prototype.add: second argument should be over 0.";
  this.tags.push(this.createTag(name, count, url, epoch));
}

TagCloud.Container.prototype.createTag = function (name, count, url, epoch) {
    return new TagCloud.Tag(name, count, url, epoch);
}

TagCloud.Container.prototype.toElement = function (filter) {
  var list = document.createElement('ul');
  for (var i = 0; i < this.tags.length; i++) {
    var tag = this.tags[i];
    if (!(filter && !filter(tag))) {
      list.appendChild(tag.toElement());
      list.appendChild(document.createTextNode("\n"));
    }
  }
  list.className = this.classes.list;
  return list;
}

TagCloud.Container.prototype.setElementsTo = function (element) {
  var target = document.getElementById(element);
  while (target.childNodes.length > 0)
    target.removeChild(target.firstChild);
  target.appendChild(this.toElement());
}

TagCloud.Container.prototype.searchAndDisplay = function (element, name) {
  var target = document.getElementById(element);
  while (target.childNodes.length > 0)
    target.removeChild(target.firstChild);
  var re = new RegExp(name, 'i');
  target.appendChild(this.toElement(function(tag){
    return tag.name.match(re);
  }));
}

TagCloud.Container.prototype.toHTML = function () {
  var temp = document.createElement('div');
  temp.appendChild(this.toElement());
  return temp.innerHTML;
}

TagCloud.Container.prototype.generateHTML = function () {
  this.runEffectors();
  return this.toHTML();
}

TagCloud.Container.prototype.setup = function (element) {
  this.runEffectors();
  this.setElementsTo(element);
}

TagCloud.Container.prototype.runEffectors = function () {
  for (var i = 0; i < this.effectors.length; i++) {
    var effector = this.effectors[i];
    effector.affect(this.tags);
  }
}

TagCloud.Container.prototype.loadEffector = function (effectorName) {
  var effectorClass = TagCloud.Effector[effectorName];
  if (!effectorClass)
    throw "Unknown Effector, " + effectorName;
  var effector = new effectorClass();
  this.effectors.push(effector);
  return effector;
}

TagCloud.Effector = new Object();

TagCloud.Effector.CountSize = function () {
  this.baseFontSize  = 24;
  this.fontSizeRange = 12;
  this._suffix       = "px";
  this.suffixTypes   = ['px', 'pt', 'pc', 'in', 'mm', 'cm'];
}

TagCloud.Effector.CountSize.prototype.base = function (size) {
  this.baseFontSize = size;
  return this;
}

TagCloud.Effector.CountSize.prototype.range = function (range) {
  this.fontSizeRange = range;
  return this;
}

TagCloud.Effector.CountSize.prototype.suffix = function (suffix) {
  for (var i = 0; i < this.suffixTypes.length; i++) {
    if (this.suffixTypes[i] == suffix) {
      this._suffix = suffix;
      break;
    }
  }
  return this;
}

TagCloud.Effector.CountSize.prototype.affect = function (tags) {
  var maxFontSize = this.baseFontSize + this.fontSizeRange;
  var minFontSize = this.baseFontSize - this.fontSizeRange;
  if (minFontSize < 0) minFontSize = 0;
  var range = maxFontSize - minFontSize;
  var min = null;
  var max = null;
  for (var i = 0; i < tags.length; i++) {
    var count = tags[i].count;
    if (min == null || min > count)
      min = count;
    if (max == null || max < count)
      max = count;
  }
  var calculator = new TagCloud.Calculator(min, max, range);
  for (var j = 0; j < tags.length; j++) {
    var tag  = tags[j];
    var size = calculator.calculate(tag.count);
    tag.style.fontSize = String(size + minFontSize) + this._suffix;
  }
}

TagCloud.Effector.DateTimeColor = function() {
  this.types = ['earliest', 'earlier', 'later', 'latest'];
  this.styles = {
    earliest: 'tagcloud-earliest',
    earlier:  'tagcloud-earlier',
    later:    'tagcloud-later',
    latest:   'tagcloud-latest'
  };
}

TagCloud.Effector.DateTimeColor.prototype.setClass = function (classes) {
  for (var prop in classes) {
    this.styles[prop] = classes[prop];
  }
  return this;
}

TagCloud.Effector.DateTimeColor.prototype.affect = function (tags) {
  var min = null;
  var max = null;
  for (var i = 0; i < tags.length; i++) {
    var epoch = tags[i].epoch;
    if (min == null || min > epoch)
      min = epoch;
    if (max == null || max < epoch)
      max = epoch;
  }
  var calculator = new TagCloud.Calculator(min, max, 3);
  for (var j = 0; j < tags.length; j++) {
    var tag   = tags[j];
    var level = calculator.calculate(tag.epoch);
    var style = this.styles[this.types[level]];
    tag.attachAnchorClass(style);
  }
}

TagCloud.Effector.HotWord = function () {
  this.hotWords  = new Array();
  this.className = 'tagcloud-hotword';
}

TagCloud.Effector.HotWord.prototype.words = function () {
  for (var i = 0; i < arguments.length; i++) {
    this.hotWords.push(arguments[i]);
  }
  return this;
}

TagCloud.Effector.HotWord.prototype.setClass = function (className) {
  this.className = className;
  return this;
}

TagCloud.Effector.HotWord.prototype.affect = function (tags) {
  for (var i = 0; i < tags.length; i++) {
    var tag = tags[i];
    for (var j = 0; j < this.hotWords.length; j++) {
      if (this.hotWords[j] == tag.name) {
        tag.initAnchorClasses();
        tag.attachAnchorClass(this.className);
        break;
      }
    }
  }
}

TagCloud.Calculator = function (min, max, range) {
  this.min    = Math.log(min);
  this.max    = Math.log(max);
  this.range  = range;
  this.factor = null;
  this.initializeFactor();
}

TagCloud.Calculator.prototype.initializeFactor = function() {
  if (this.min == this.max) {
    this.min -= this.range;
    this.factor = 1;
  } else {
    this.factor = this.range / (this.max - this.min);
  }
}

TagCloud.Calculator.prototype.calculate = function (num) {
  return parseInt((Math.log(num) - this.min) * this.factor);
}

