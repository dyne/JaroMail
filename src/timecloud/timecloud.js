/*
 * jQuery UI Timecloud
 *
 * Copyright (c) 2008-2009 Stefan Marsiske
 * Dual licensed under the MIT and GPLv3 licenses.
 *    Copyright (C) 2008  Stefan Marsiske
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * http://github.com/stef/timecloud/
 *
 * Depends:
 *	jquery: ui.core.js, ui.draggable.js, ui.slider.js, jquery.sparkline
 *	external: tagcloud.js
 */

(function($) {
   $.widget("ui.timecloud", {
      ui: function(e) {
         return {
            options: this.options,
         };
      },

      // loads a sparse list of timed tagclouds, fills empty days with empty
      // tagclouds, then builds the appropriate dom and finally draws the first frame
      // afterwards it starts the animation if necessary
      _init: function() {
          this.sparkline = [];
          this.tags = [];
          this.overview = [];
          this.frames = [];

          // data received can be sparse,
          // - we fill any missing timesegments with empty data
          // - calculate data to be displayed on the overwiew sparkline
          var nextdate = this.strToDate(this.options.timecloud[0][0]);
          for (id in this.options.timecloud) {
              var curdate = this.strToDate(this.options.timecloud[id][0]);
              while(nextdate && nextdate < curdate) {
                  this.frames.push([this.dateToStr(nextdate), []]);
                  nextdate = this.addDay(nextdate,1);
              }
              nextdate = this.addDay(nextdate,1);
              // push non-sparse data
              this.frames.push([this.options.timecloud[id][0], this.options.timecloud[id][1]]);

              // calculate overview counts
              curDay = this.options.timecloud[id][1];
              var tag;
              var cnt = 0;
              for (tag in curDay) {
                  cnt += parseInt(curDay[tag][1]);
              }
              this.overview.push({'date': this.options.timecloud[id][0], 'count': cnt});
          }
          // calculate window position if options.start=-1
          if(0 > this.options.start) {
              this.options.start = this.frames.length - this.options.winSize + (this.options.start + 1);
              // no sense in playing forward
              this.options.playBack=true;
          }

          // visualize the data
          if(this.options.winSize != 0) {
              if(this.options.winSize>this.frames.length) {
                  this.options.winSize=this.frames.length;
              }
              this.buildWidget();
              this.drawTimecloud();
              this.play();
          } else {
              // if initial winSize is 0, then we start in tagcloud mode
              this.options.winSize = this.frames.length;
              this.buildWidget();
              this.drawTimecloud();
              // we hide the animation related elements
              this.timecloudElem.hide();
          }
      },

      play: function() {
          var self=this;
          if(! this.options.play) return;
          if(this.options.playBack) {
              setTimeout(function() { self.prevFrame.call(self); }, this.options.timeout);
          } else {
              setTimeout(function() { self.nextFrame.call(self); }, this.options.timeout);
          }
      },

      // internal, used to build the DOM
      buildWidget: function() {
          var thisObj = this;
          this.element.addClass("timecloud");

          // build the overview area: a sparkline with a window to select
          // the range of displayed data
          this.overviewElem = this.buildOverview();
          this.element.append(this.overviewElem);

          // build the detail area: 
          // * an optional zoomed in portion of the overview is displayed,
          // * an optional playback controls and
          // * and of course the tagcloud
          this.timecloudElem=$("<div/>").addClass("details");
          // we setup a timeline graph of only the currently shown tags
          this.timecloudElem.append(this.buildSparkline());
          // we want the mousewheel events to scroll the window
          this.timecloudElem.bind('wheel', function(e) {
              if(e.delta<0) {
                  thisObj.nextFrame();
              } else {
                  thisObj.prevFrame();
              }})

          // building the animation controls
          // setup controls for time window size
          this.controls = this.buildControls().appendTo(this.timecloudElem);

          this.element.append(this.timecloudElem);

          // create container for tagcloud
          $("<div/>").addClass("tagcloud")
          .appendTo(this.element);
      },

      // builds a sparkline with a sliding window, the sparkline
      // displays the sum of weights over time
      buildOverview: function() {
          var thisObj=this;

          var elem=$("<div/>").addClass("overview")
          .bind('wheel', function(e) { thisObj.overviewScrolled(e);});

          // you can pan/zoom the timecloud using a window on the overview
          // sparkline
          var timegraph=this.buildSparklineRange({
                  values: [parseInt(this.options.start),this.options.start+this.options.winSize-1],
                  min: 0,
                  max: this.frames.length,
                  orientation: 'horizontal',
                  range: true,
                  stop: function(e,ui) {
                     return thisObj.overviewZoomed(e,ui)},
              },
              function(e,ui) { return thisObj.overviewMoved(e,ui)});

          elem.append(timegraph);
          elem.window=$('.ui-slider',elem);
          return elem;
      },

      // internal: used to build a windowed sparkline
      buildSparklineRange: function(sliderOpts,dragCb) {
          var thisObj=this;

          // create an overlay and two handles inside
          var window = $("<div/>");
          $("<span/>").addClass("ui-slider-handle")
          .addClass("left")
          .appendTo(window);
          $("<span/>").addClass("ui-slider-handle")
          .addClass("right")
          .appendTo(window);
          // initialize ui-slider
          window.slider(sliderOpts)
          // we also add support for dragging the window
          .find(".ui-slider-range").draggable({
              axis: 'x',
              containment: '.ui-slider',
              helper: 'clone',
              stop: function(e,ui) { dragCb(e,ui); }, 
              });
          return this.buildSparkline().append(window);
      },

      // internal: used when building the UI
      buildSparkline: function(e) {
          var timegraph = $("<div/>").addClass("timegraph");
          var container = $("<div/>").addClass("sparkline-container").appendTo(timegraph);
          var labels = $("<div/>").addClass("sparkline-label").appendTo(container);
          $("<div/>").addClass("max")
          .appendTo(labels);
          $("<div/>").addClass("min")
          .appendTo(labels);

          $("<div/>").addClass("sparkline")
          .appendTo(container);

          var dates=$("<div/>").addClass("dates").appendTo(container);
          $("<span/>").addClass("startdate").
          appendTo(dates);
          $("<span/>").addClass("enddate").
          appendTo(dates);

          return timegraph;
      },

      // internal fn: builds controls
      buildControls: function() {
          var thisObj=this;

          var result = $("<div />")
          .addClass("control-container");
          // play direction back
          result.back=$('<span>&lt;</span>')
          .addClass("text-control")
          .click(function () {
              thisObj.options.playBack=true;
              $(this).siblings().removeClass("selected");
              $(this).addClass("selected");
          })
          .appendTo(result);
          // play/pause
          result.playElem = $('<span>Play</span>')
          .addClass("text-control")
          .click(function () { $(this).text(thisObj.togglePlay()); })
          .appendTo(result);
          // forward direction
          result.forward = $('<span>&gt;</span>')
          .addClass("text-control")
          .click(function () {
              thisObj.options.playBack=false;
              $(this).siblings().removeClass("selected");
              $(this).addClass("selected");
          })
          .appendTo(result);
          if(this.options.playBack) {
              result.back.addClass("selected");
          } else {
              result.forward.addClass("selected");
          }

          // animation speed slider
          result.speed = $("<div/>").addClass("speed");
          $("<span>normal...fast</span>").addClass("ui-speed-label")
          .appendTo(result.speed);

          result.speed.slider({
              min: 0,
              orientation: 'horizontal',
              step: 1,
              max: 2,
              change: function (e,ui) {
                  if(ui.value == 0) {
                      thisObj.options.steps=1;
                  } else if(ui.value == 1) {
                      thisObj.options.steps=Math.round(thisObj.options.winSize*0.1);
                  } else if(ui.value == 2) {
                      thisObj.options.steps=Math.round(thisObj.options.winSize*0.2);
                  }
              } });
          result.speed.appendTo(result);
          return result;
      },

      // callback changing the range in the overview sparkline using the sliders
      overviewZoomed: function (e, ui) {
          var left=this.overviewElem.window.slider('values',0);
          var range=this.overviewElem.window.slider('values',1) - left;
          if(this.options.winSize == this.frames.length && range < this.frames.length) {
              this.timecloudElem.slideDown();
          } else if(this.options.winSize < this.frames.length && range >= this.frames.length) {
              this.timecloudElem.slideUp();
          }
          this.options.start = left;
          this.options.winSize = Math.round(range);
          this.drawTimecloud();
      },

      // callback for the draggable ui-slider-range
      overviewMoved: function (e, ui) {
          this.options.start = Math.round(
              (this.frames.length*ui.position.left) / this.element.width())
          this.drawTimecloud();
          return false;
      },

      // callback used on mouse scrollwheel events in the overview area
      overviewScrolled: function(e) {
          var delta = (Math.round(this.frames.length / 100) * e.delta * - 1);

          if(this.options.winSize + delta > 0 && this.options.start - Math.round(delta / 2) >= 0 &&
             (this.options.start + this.options.winSize + Math.round(delta / 2)) <= this.frames.length) {
              if(this.options.winSize == this.frames.length && this.options.winSize + delta < this.frames.length) {
                  this.timecloudElem.slideDown();
              } else if(this.options.winSize < this.frames.length && this.options.winSize + delta >= this.frames.length) {
                  this.timecloudElem.slideUp();
              }
              this.options.winSize = this.options.winSize + delta;
              this.options.start = this.options.start - Math.round(delta / 2);

              this.drawTimecloud();
          }
      },

      // wrapper for redrawRange
      redrawOverviewRange: function() {
          var left=parseInt(this.options.start);
          //this.overviewElem.window.slider('option','values',[left,left + this.options.winSize]);
          this.overviewElem.window.slider('values', 0, left);
          this.overviewElem.window.slider('values', 1 ,left + this.options.winSize);
      },

      // internal: used to draw a fresh frame
      drawTimecloud: function() {
          this.initCache();
          this.redrawTimecloud();
      },

      // internal: calculates a tagcloud from window_size elems in frame
      // it updates the sparkline cache as well
      initCache: function () {
          var i = this.options.start;
          this.tags = [];
          this.sparkline = [];
          // iterate over winSize
          while(i < this.options.start + this.options.winSize) {
              // fetch current day
              if(i > this.frames.length - 1) break;
              var curday = this.frames[i];
              var currentDate = curday[0];
              //iterate over tags in day
              var item;
              var cnt = 0;
              for(item in curday[1]) {
                  var tag = curday[1][item][0];
                  var count = parseInt(curday[1][item][1]);
                  if(this.tags[tag]) {
                      // add count
                      this.tags[tag].count += count;
                  } else {
                      // add tag
                      this.tags[tag] = [];
                      this.tags[tag].count = count;
                  }
                  this.tags[tag].currentDate = currentDate;
                  cnt += count;
              }
              this.sparkline.push({'date': currentDate, 'count': cnt});
              i+=1;
          }
      },

      // internal: this draws a tagcloud and sparkline from the cache
      redrawTimecloud: function() {
          this.drawTagcloud(this.listToDict(this.tags), this.element);
          this.drawSparkline(this.overview, this.overviewElem);
          this.drawSparkline(this.sparkline, this.timecloudElem);
          this.redrawOverviewRange();
      },

      // internal: used to all draw sparklines, we need to expand the possibly
      // sparse list of data and loose btw the dates in this process, in the end
      // we also display the start and end date on the left/right below the
      // sparkline
      drawSparkline: function (data,target) {
          // data might be sparse, insert zeroes into list
          var startdate = this.strToDate(data[0]['date']);
          var enddate = this.strToDate(data[data.length-1]['date']);
          var nextdate = startdate;
          var lst = [];
          var min = Infinity;
          var max = -Infinity;
          for (id in data) {
              var curdate = this.strToDate(data[id]['date']);
              while(nextdate<curdate) {
                  lst.push(0);
                  nextdate = this.addDay(nextdate,1);
              }
              var val = parseInt(data[id]['count']);
              if(val>max) max = val;
              if(val<min) min = val;
              lst.push(val);
              nextdate = this.addDay(nextdate,1);
          }
          $('.min',target).text(min);
          $('.max',target).text(max);
          $('.startdate',target).text(this.dateToStr(startdate));
          $('.enddate',target).text(this.dateToStr(enddate));
          var tmp = this.options.sparklineStyle;
          tmp.width = $('.sparkline',target).width();
          $('.sparkline',target).sparkline(lst, tmp);
          $.sparkline_display_visible()
      },

      // internal: this is used to draw a tagcloud, we invoke the services of tagcloud.js
      drawTagcloud: function (data,target) {
          var tc;
          var url = '';
          tc = TagCloud.create();
          for (id in data) {
              var timestamp;
              if(data[id][2]) {
                  timestamp = this.strToDate(data[id][2]);
              }
              if(this.options.urlprefix || this.options.urlpostfix) {
                  url = this.options.urlprefix+data[id][0]+this.options.urlpostfix; //name
              }
              if(parseInt(data[id][1]) ) {
                  // name
                  tc.add(data[id][0],
                         // count
                         parseInt(data[id][1]),
                         url,
                         timestamp); // epoch
              }
          }
          tc.loadEffector('CountSize').base(24).range(12);
          tc.loadEffector('DateTimeColor');
          tc.runEffectors();
          $(".tagcloud", target).empty().append(tc.toElement());
      },

      // internal: used as a callback for the play button
      togglePlay: function() {
          if(this.options.play) { this.options.play = false; return("Play"); }
          else { this.options.play = true; this.play(); return("Pause");}
      },

      // internal: updates the cache advancing the window by self steps. to save
      // time we subtract only the removed days tags and add the added days tags
      // to the cache. afterwards we update the sliding window widget, redraw the
      // timecloud and time the next frame
      nextFrame: function () {
          if(this.options.start+this.options.winSize+this.options.steps<=this.frames.length) {
              var self = this;
              // subtract $steps frames from $tags and $sparkline
              var exclude = this.frames.slice(this.options.start, this.options.start+this.options.steps);
              this.delFromCache(exclude);
              this.sparkline.splice(0,this.options.steps);

              // add $steps framse to tags and sparkline
              var include = this.frames.slice(this.options.start+this.options.winSize, this.options.start+this.options.winSize+this.options.steps);
              this.sparkline = this.sparkline.concat(this.addToCache(include));

              // advance $start by $steps
              this.options.start+=this.options.steps;

              // draw timecloud (current frame)
              this.redrawTimecloud();
              this.play();
          } else {
              this.options.play = false;
              this.controls.playElem.text("Play");
          }
      },

      prevFrame: function () {
          if(this.options.start-this.options.steps>=0) {
              var self = this;
              // subtract $steps frames from $tags and $sparkline
              var exclude = this.frames.slice(this.options.start+this.options.winSize-this.options.steps, this.options.start+this.options.winSize);
              this.delFromCache(exclude);
              this.sparkline.splice(this.sparkline.length-this.options.steps,this.options.steps);

              // add $steps framse to tags and sparkline
              var include = this.frames.slice(this.options.start-this.options.steps, this.options.start);
              this.sparkline = this.addToCache(include).concat(this.sparkline);

              // advance $start by $steps
              this.options.start-=this.options.steps;

              // draw timecloud (current frame)
              this.redrawTimecloud();
              this.play();
          } else {
              this.options.play = false;
              this.controls.playElem.text("Play");
          }
      },

      addToCache: function(frames) {
          var thisObj = this;
          var sparkline = [];
          // we need to add each days tags to the cache
          frames.forEach(function(day) {
              var today = day[0];
              var cnt = 0;
              day[1].forEach(function(tag) {
                  if(thisObj.tags[tag[0]]) {
                      thisObj.tags[tag[0]].count+=parseInt(tag[1]);
                  } else {
                      thisObj.tags[tag[0]] = new Array();
                      thisObj.tags[tag[0]].count = parseInt(tag[1]);
                  }
                  cnt+=parseInt(tag[1]);
                  thisObj.tags[tag[0]].currentDate = today;
              });
              sparkline.push({'date': today, 'count': cnt});
          });
          return sparkline;
      },

      delFromCache: function(frames) {
          var thisObj = this;
          frames.forEach(function(day) {
              day[1].forEach(function(tag) {
                  thisObj.tags[tag[0]].count-=parseInt(tag[1]);
                  if(thisObj.tags[tag[0]].count<=0) {
                      thisObj.tags.splice(thisObj.tags.indexOf(tag[0]),1);
                  }
              });
          });
      },

      // internal: used to convert the cache to the tagcloud.js format
      listToDict: function (lst) {
          var dict = [];
          // convert tags into list for drawTagcloud
          for ( tag in lst) {
              dict.push([tag, lst[tag].count, lst[tag].currentDate]);
          }
          return dict;
      },

      // internal: helper function to cope with dates
      dateToStr: function (dat) {
          var d  = dat.getDate();
          var day = (d < 10) ? '0' + d : d;
          var m = dat.getMonth() + 1;
          var month = (m < 10) ? '0' + m : m;
          var yy = dat.getYear();
          var year = (yy < 1000) ? yy + 1900 : yy;
          return(year + "-" + month + "-" + day);
      },

      // internal: helper function to cope with dates
      strToDate: function (str) {
          var frgs = str.split("-");
          return(new Date(frgs[0],frgs[1]-1,frgs[2]));
      },

      // internal: helper function to cope with dates
      addDay: function (d,n) {
          var oneday = 24*60*60*1000;
          return new Date(d.getTime() + n*oneday);
      },
    });

    $.ui.timecloud.getter = "start winSize steps timeout play graphStyle";
    $.ui.timecloud.defaults = {
        timecloud: [], // the raw(sparse) timecloud data
        start: 0, // first frame to show, negative values start at the end-winSize
        winSize: 30, // 0 sets the window to span the whole dataset
        steps: 1, // animation should advance this many days / frame
        timeout: 200, // delay between frames
        playBack: false,  // forward
        play: false,  // start playing?
        sparklineStyle: { type:'line', lineColor:'Navy', height:'30px', chartRangeMin: '0' },
        urlprefix: '', // tagcloud links will be pointing here
        urlpostfix: '' // tagcloud links get this postfix
    };
})(jQuery);
