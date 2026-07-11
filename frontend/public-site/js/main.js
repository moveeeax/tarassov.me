/*
	Name: BookCard
	Description: Responsive HTML5 vCard Template
	Version: 1.2
	Author: pixelwars
*/


(function ($) {

	var safeMod = false;
	var portfolioKeyword;
	
	/* DOCUMENT LOAD */
	$(function() {
		
		
		
// ------------------------------
		
		
		
		// ------------------------------
		// PORTFOLIO DETAILS
		// if url contains a portfolio detail url
		portfolioKeyword = $('.portfolio').attr('id');
		initialize();
		// ------------------------------
						
				
		// ------------------------------
		// SET COVER IMAGE AS BG IMAGE
		$('.cover').each(function(index, element) {
			$(this).css('background-image', 'url(' + $(this).find('.cover-image-holder img').attr('src') + ')');
		});
		// ------------------------------
		
			
		// ------------------------------
		// LAYOUT FALLBACK : SAFE MOD
		safeMod = $('html').attr('data-safeMod') === 'true';
		var isIE11 = !!navigator.userAgent.match(/Trident\/7\./); 
		var ua = navigator.userAgent.toLowerCase();
		var isAndroid = ua.indexOf("android") > -1;
		var isOperaMobile = isAndroid && navigator.userAgent.indexOf("Opera")> -1;
		safeMod = safeMod || !Modernizr.csstransforms || !Modernizr.csstransforms3d || $(window).width() < 960 || $.browser.msie || isIE11 || isAndroid || isOperaMobile;
		if(safeMod) {
			
			$('html').addClass('safe-mod');	
			
			setActivePage();
			$.address.change(function() {
				setActivePage();
				});
		}
		// ------------------------------
		
		
		// ------------------------------
		// open the book if url contains #/portfolio
		if (($.address.path().indexOf("/"+ portfolioKeyword)!=-1) && !safeMod) {
			setTimeout(function() { openMenu() },500);
		}
		// ------------------------------
		
		
		// ------------------------------
		// ADAPT LAYOUT
		adaptLayout();
		$(window).resize(function() {
			adaptLayout();
		});
		// ------------------------------
		
		
// ------------------------------
		
		
		
		
// ------------------------------
		
		
		
		
		// ------------------------------
		// SCROLLBARS
		if(!safeMod) {
			
			setupScrollBars();
	
			// REFRESH SCROLLBARS ON RESIZE
			$(window).resize(function() {
				refreshScrollBars();
				if($(window).width() < 960) {
					location.reload(true);	
				}
			});
		
		}
		// ------------------------------
	
		
		// ------------------------------
		// SAFE-MOD SECTION FLOW (mobile/tablet only — the desktop book keeps
		// its own antiscroll panels). Scrolling past the end of a main section
		// advances to the next one (resume -> portfolio -> contact); scrolling
		// up from the very top goes back. The cover stays out of the cycle.
		if (safeMod && $('#rm-container').length) {
			(function() {
				var order = ['resume', 'portfolio', 'contact'];
				var lockUntil = 0;
				var touchY = null, startedAtTop = false, startedAtBottom = false;

				function scrollTopNow() {
					return window.pageYOffset || document.documentElement.scrollTop || 0;
				}
				function atTop() { return scrollTopNow() <= 2; }
				function atBottom() {
					var doc = document.documentElement;
					var full = Math.max(document.body.scrollHeight, doc.scrollHeight);
					return window.innerHeight + scrollTopNow() >= full - 2;
				}
				function go(step) {
					var i = order.indexOf($('.page.active').attr('id'));
					if (i === -1) return; // on the cover or an unknown page
					var next = i + step;
					if (next < 0 || next >= order.length) return;
					var now = new Date().getTime();
					if (now < lockUntil) return;
					lockUntil = now + 900;
					$.address.path(order[next]); // setActivePage animates the swap
				}

				window.addEventListener('wheel', function(e) {
					if (e.deltaY > 0 && atBottom()) go(1);
					else if (e.deltaY < 0 && atTop()) go(-1);
				}, { passive: true });

				window.addEventListener('touchstart', function(e) {
					touchY = e.touches[0].clientY;
					startedAtTop = atTop();
					startedAtBottom = atBottom();
				}, { passive: true });
				window.addEventListener('touchend', function(e) {
					if (touchY === null) return;
					var dy = touchY - e.changedTouches[0].clientY; // >0: swipe up = scroll down
					if (dy > 70 && startedAtBottom) go(1);
					else if (dy < -70 && startedAtTop) go(-1);
					touchY = null;
				}, { passive: true });
			})();
		}
		// ------------------------------
	
		
		// ------------------------------
		// FIT TEXT
		fitText();
		// ------------------------------
		
		
		// ------------------------------
		// 3D LAYOUT
		Menu.init();
		// ------------------------------

		// Reveal the page after layout init (pairs with FOUC guard in main.css
		// and early .safe-mod from js/boot.js). rAF waits one paint so transforms
		// are applied before opacity goes to 1.
		requestAnimationFrame(function () {
			document.documentElement.classList.add('is-ready');
		});
		
		
	});
	// DOCUMENT READY
	
	
	
	
	// WINDOW ONLOAD
	window.onload = function() {
		
		// ie8 cover text invisible fix
		if(jQuery.browser.version.substring(0, 2) == "8." || jQuery.browser.version.substring(0, 2) == "7.")
		{
			setTimeout(function() { setActivePage(); },2000);	
		}
		
		setTimeout(function() { fitText(); },1000);	
	
	};
	// WINDOW ONLOAD	
	
	
	// ------------------------------
	// ------------------------------
		// FUNCTIONS
	// ------------------------------
	// ------------------------------
	
	
	// ------------------------------
	// INITIALIZE
	var safeModPageInAnimation, cover_h1_tune, cover_h2_tune, cover_h3_tune, cover_h3_span_tune;
	function initialize() {
		safeModPageInAnimation = $('html').attr('data-safeModPageInAnimation');
		cover_h1_tune = $('html').attr('data-cover-h1-tune');
		cover_h2_tune = $('html').attr('data-cover-h2-tune');
		cover_h3_tune = $('html').attr('data-cover-h3-tune');
		cover_h3_span_tune = $('html').attr('data-cover-h3-span-tune');
	}
	// ------------------------------
	
	
	
	// ------------------------------
	// ADAPT LAYOUT
	function adaptLayout() {
		var width = safeMod ? $('body').width() : $('.content').width();
		if(width < 420) {
			$('html').addClass('w420');	
		} else {
			$('html').removeClass('w420');		
		}
	}	
	// ------------------------------
	
	
	// ------------------------------
	// CHANGE PAGE
	function setActivePage() {
		
			$('.page').removeClass('active').hide();
			var path = $.address.path();
			path = path.slice(1, path.length);
			if(path == "") {  // if hash tag doesnt exists - go to first page
				var firstPage = $('#header ul li').first().find('a').attr('href');
				path = firstPage.slice(2,firstPage.length);
				$.address.path(path);
				}
			
			if(Modernizr.csstransforms && Modernizr.csstransforms3d) { // modern browser
				$('#'+ path).show().removeClass('animated ').addClass('animated ' + safeModPageInAnimation);
			} else { //old browser
				$('#'+ path).fadeIn();
				$('.page.active').hide();
			}	
			
			$('#'+ path).addClass('active');
			
			// detect if user is on cover page
			if($('.page.active').find('.cover').length) {
				$('html').removeClass('not-on-cover-page').addClass('on-cover-page');	
			} else {
				$('html').removeClass('on-cover-page').addClass('not-on-cover-page');	
			}
			
			setCurrentMenuItem();
			
			$("body").scrollTop(0);
			
			fitText();
		
	}	
	// ------------------------------
	
	
	// ------------------------------
	// FIT TEXT
	function fitText() {
		$(".cover h1").fitText(cover_h1_tune);
		$(".cover h2").fitText(cover_h2_tune);
		$(".cover h3").fitText(cover_h3_tune);
		$(".cover h3 span").fitText(cover_h3_span_tune);	
	}
	// ------------------------------
	
	
	// ------------------------------
	// SCROLLBARS
	var scroller = [];
	
	// SETUP SCROLLBARS
	function setupScrollBars() {
		if(!safeMod) { // don't run antiscroll if safe mode is on 
			
			$('.antiscroll-wrap').each(function(index, element) {
				scroller[index] = $(this).antiscroll( { x : false, autoHide: $('html').attr('data-autoHideScrollbar') != 'false' }).data('antiscroll');
			});
			
		}
	}
	
	// REFRESH SCROLLBARS
	function refreshScrollBars() {
		 setTimeout(function() { rebuildScrollBars(); setupScrollBars(); }, 500);
	}
	
	// REBULD SCROLLBARS
	function rebuildScrollBars() {
		 $.each( scroller, function(i, l){
			scroller[i].rebuild(); 
		 });
	}
	// ------------------------------
	
	
	// ------------------------------
	// SET CURRENT MENU ITEM
	function setCurrentMenuItem() {
		var activePageId = $('.page.active').attr('id');
		// set default nav menu
		$('#header nav ul a[href$=' + activePageId +']').parent().addClass('current_page_item').siblings().removeClass('current_page_item');
	}	
	// ------------------------------
	
	
	// ------------------------------
	// BOOK LAYOUT
	var Menu = (function() {
		
		var $container = $( '#rm-container' ),						
			$cover = $container.find( 'div.rm-cover' ),
			$middle = $container.find( 'div.rm-middle' ),
			$right = $container.find( 'div.rm-right' ),
			$open = $cover.find('a.rm-button-open'),
			$close = $right.find('.rm-close');
	
			init = function() {
	
				initEvents();
	
			},
			initEvents = function() {
	
				$open.on( 'click', function( event ) {
					if(!safeMod) {
		
						openMenu();
						return false;
					}
	
				} );
	
				$close.on( 'click', function( event ) {
					
					closeMenu();
					return false;
	
				} );
				
			},
			openMenu = function() {
	
				$container.removeClass('rm-closed');
				setTimeout(function() { $container.addClass( 'rm-open' ); },10);
	
			},
			closeMenu = function() {
	
				$container.removeClass( 'rm-open rm-nodelay rm-in' );
				setTimeout(function() { $container.addClass( 'rm-closed' ) },850);
	
			};
			
		return { init : init };
	
	})();
	// ------------------------------


})(jQuery);