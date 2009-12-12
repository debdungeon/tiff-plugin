/*
 * Mozilla-tiff plugin
 *
 * mozilla-tiff-viewer.c
 *
 * Copyright (C) 2006
 *
 * Developed by Vinay Ramdas based on mozilla-bonobo by Christian Glodt
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */

#include <gtk/gtk.h>
#include <glib.h>
#include <glib/gslist.h>
#include <gdk/gdkx.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <string.h>
#include <stdio.h>

/* Function for printing debugging messages */
void DEBUGM(const char* format, ...)
{
#ifdef DEBUG
	va_list args;
	va_start(args, format);	
	g_printerr(format, args);
	va_end(args);
#endif
}


/* The URL of the document. *Not* used except for display. */
char *url, local_name[400], local_cache_dir[300], current_filename[400];

/* The mime type of the document. */
char *mime_type;

/* The io channel through which the viewer receives commands from the plugin */
GIOChannel *in_channel;

/* The io channel through which the viewer sends commands to the plugin */
GIOChannel *out_channel;

/* The GtkPlug that embeds the viewer into the browser window */
GtkWidget *plug;	/* Main "window" */
GtkWidget *scrolledWindow, *InnerWindow;
GtkObject *viewerHAdjustment, *viewerVAdjustment;


/* The GtkFixed that is used to get sane resizing */
GtkWidget *fixed;

/* The progress bar that's shown during the download phase */
GtkWidget *progress;

/* The identifier of the timeout that's used to update the activity bar */
guint timeout;

gboolean timeout_stop = FALSE;

GtkWidget *widget;	/* The bonobo control's widget */
GtkWidget *control;
GdkPixbuf *pixbuf, *original_image, *current_image;
GdkPixbufAnimation *animated_image;
GdkPixbufAnimationIter *image_iterator; 

/* The parent window of the plugin (ie the "socket") that we
   got passed as an XID from the plugin */
GdkWindow *gdk_parent;


/* TRUE if in embedded mode, FALSE if standalone */
int embedded;

/* The size that we got passed from the plugin */
int width, height, imgwidth, imgheight, w_width, w_height, tiff_pages, current_page=0;
float ratio;

/* The control that is currently displayed.
   NOTE: This must only be set using the setMainControl function!!! */
GtkWidget *actual_control = NULL;


GtkWidget *createPlug(unsigned long xid)
{
	GtkWidget *res;
	
	/* This plugs into the browser */
	res = gtk_plug_new(xid);    

	/* Get the GdkWindow too */
	gdk_parent = gdk_window_foreign_new(xid);
	
	/* Set the initial size */
	gdk_window_get_geometry(gdk_parent, NULL, NULL, &w_width, &w_height, NULL);

	gtk_window_set_default_size(GTK_WINDOW(res), w_width, w_height);

	/* Important: realize the widget so it gets its own X window */
	gtk_widget_realize(res);

	/* Do some reparenting black magic */
	XReparentWindow(GDK_WINDOW_XDISPLAY(res->window),
		GDK_WINDOW_XID(res->window),
		xid, 0, 0);
	XMapWindow(GDK_WINDOW_XDISPLAY(res->window),
		GDK_WINDOW_XID(res->window));

	return res;
}

/* This function is the only one that should be used to set the main
 * control! It removes any previously set control.
 */
void setMainControl(GtkWidget *w)
{
	
	if (actual_control) {
		gtk_container_remove(GTK_CONTAINER(fixed), GTK_WIDGET(actual_control));
	}
	
	gtk_widget_set_usize(GTK_WIDGET(w), width, height);
	
	gtk_fixed_put(GTK_FIXED(fixed), GTK_WIDGET(w), 0, 0);
	
	gtk_widget_show(GTK_WIDGET(w));
	
	actual_control = w;
}

/* Callback called if the window is closed. Applicable only in standalone mode.
 */
static void window_destroyed (GtkWindow *window, char * data)
{
	gtk_main_quit ();
}


int split_multi_tiff(char *filename)
{
   FILE *pipe;
   char command[1000], line[10];
   int pages=1;
   if (!g_file_test(filename, G_FILE_TEST_EXISTS))
      return( pages);
   snprintf(command, 1000, "tiffinfo %s | grep -i 'tiff directory'| wc -l",filename);
   if ( !(pipe = (FILE*)popen(command,"r")) )
      return(pages);
   
   fgets( line, sizeof(line), pipe);
   pclose(pipe);
   if (atoi(line) > 0 )
      pages = atoi(line);
   bzero(command, 1000);
   if (pages > 1)
   {
      snprintf(command, 1000, "tiffsplit %s %s",filename,filename);
      system(command);
   }
   return pages;
}


int current_page_filename(int page)
{
   char last, second;
   if (tiff_pages == 1)
      return 0;

   last = 97 + (page % 26);
   second = 97 + ((page / 26) > 26?26:(page / 26));
   
   
   snprintf(current_filename, 400,"%sa%c%c.tif",url,second,last);
//   g_print ("current filename : %s\n", current_filename);
   g_object_unref(original_image);
   g_object_unref(pixbuf);
   original_image = gdk_pixbuf_new_from_file(current_filename , NULL);
   pixbuf = gdk_pixbuf_scale_simple( original_image, imgwidth, imgheight, GDK_INTERP_BILINEAR);
   return 0;
}

/* This function is called as soon as data is available on the io channel
 * on which we receive commands from the plugin.
 */
static gboolean io_func(GIOChannel *source, GIOCondition condition, gpointer data)
{
	GError *err = NULL;
	
	char *string;	/* The string we read from the io channel */

	gsize length;	/* Its size */
	
	/* Read 1 line from the io channel, including newline character */
	g_io_channel_read_line(source, &string, &length, NULL, &err);

	if (err != NULL)
	{
		/* Whoops, there's been an error */
		g_error("Error: %s\n", err->message);
		g_error_free(err);
		/* Try again next time */
		return TRUE;
	}
	
	/* Any of these conditions makes us ignore the string */
	if (length == 0) return TRUE;
	if (string == 0) return TRUE;
	
	DEBUGM("viewer: io_func(%s)", string);

	if (g_str_equal("size\n", string) == TRUE)
	{
		/* The command was "size" */
		int w, h;
		
		g_free(string);
		
		/* Read the new width */
		g_io_channel_read_line(source, &string, &length, NULL, NULL);
		w = atoi(string);
		g_free(string);
		
		/* Read the new height */
		g_io_channel_read_line(source, &string, &length, NULL, NULL);
		h = atoi(string);
		g_free(string);
		
		DEBUGM(" %i, %i", w, h);
		
		/* If the size actually changed */
		if (w != width || h != height)
		{
		
			width = w;
			height = h;
			
			/* Set it */
			gtk_widget_set_usize(GTK_WIDGET(plug), w, h);
			gtk_widget_set_usize(GTK_WIDGET(actual_control), w, h);
			
			/* If the parent is known, try to resize it */
			if (gdk_parent)
			{
				gdk_window_resize(GDK_WINDOW(gdk_parent), w, h);
			}
		}
		return TRUE;
 	}

	if (g_str_equal("filename\n", string) == TRUE)
	{
		/* The command was "filename". This command indicates that
		   downloading of the file is finished, and passes us the
		   "file:///" url of the file in mozillas cache */
		
		char *stripped_name ;
		
		/* In any case, make the timeout stop next time */
		timeout_stop = TRUE;
		
		g_free(string);
		
		/* Read the file URL */
		g_io_channel_read_line(source, &string, &length, NULL, NULL);
		
		/* Strip off the newline */
		stripped_name = g_strstrip(string);
		
		DEBUGM(" %s", stripped_name);

		//local_name = g_string_erase(&stripped_name, 0, 7);
		//local_name = stripped_name + 7;
		strncpy(local_name, &stripped_name[7], 400);
		url = local_name;
		/* Create a control that can display the URL */
		

		gdk_pixbuf_get_file_info(url, &imgwidth, &imgheight);
		ratio = imgwidth/imgheight;

		//		imgheight = height;
		//		imgwidth = height*ratio;
		
		imgwidth = w_height*imgwidth/imgheight;
		imgheight = w_height;
		width = 0;
		height = 0;

		tiff_pages = split_multi_tiff(local_name);
		original_image = gdk_pixbuf_new_from_file(url , NULL);
		
//		pixbuf = gdk_pixbuf_new_from_file_at_scale(url, imgwidth, imgheight, TRUE,NULL);
		pixbuf = gdk_pixbuf_scale_simple( original_image, imgwidth, imgheight, GDK_INTERP_BILINEAR);
		control=gtk_image_new_from_pixbuf(pixbuf);
		//control = gtk_image_new_from_file(url);
		if ((control == NULL) || (original_image == NULL))
		{
			/* The control failed to load */
			char *msg;
						
			/* Set up the label */
			//			msg = g_strdup_printf("File has been downloaded and filename is %s- width: %d   height: %d  ratio: %f newwidth: %d newheight: %d", local_name, imgwidth, imgheight, ratio, height*imgwidth/imgheight, height );
			msg = g_strdup_printf("\n\n\n\n\n\tHmmmmm..... File seems to be corrupted!!");
//			msg = g_strdup_printf("\n\n\n\n\n\tfilename is %s\n no of tiff pages  %d",local_name,tiff_pages);
			control = gtk_label_new(msg);
			gtk_label_set_line_wrap(GTK_LABEL(control), TRUE);
			gtk_label_set_justify(GTK_LABEL(control), GTK_JUSTIFY_CENTER);
			g_free(msg);

			setMainControl(control);
			
		}
		else
		{
			/* The control got correctly instantiated */
			setMainControl(control);			
		}
		g_free(string);
		return TRUE;
	}
	
	if (g_str_equal("exit\n", string) == TRUE)
	{
		unsigned int nb;
		/* The command was "exit", so that's what we do */
		
		g_free(string);
		
		g_io_channel_write (out_channel, "exit\n", 5, &nb);
		
		gtk_main_quit();
		return TRUE;
	}
	
	if (g_str_equal("param\n", string) == TRUE)
	{
		/* Getting a param to pass to the control */
		
		char *name, *end;
		double d;
		int i;
		guint64 u;
		
		g_free(string);
		/* First, retrieve the param name */
		g_io_channel_read_line(source, &string, &length, NULL, NULL);
		name = g_strdup(string);
		name[strlen(name) - 1] = 0;/*to remove the \n*/
		g_free (string);
		
		/* Now, get the value */
		g_io_channel_read_line(source, &string, &length, NULL, NULL);
		string[strlen(string) - 1] = 0;/*to remove the \n*/
		g_free (name);
		
		return TRUE;
	}

	if (g_str_equal("reparent\n", string) == TRUE)
	{
		/* The command was reparent */
		unsigned long new_xid;
		GtkWidget *dialog;
		int ret;
		
		g_free(string);
		
		/* Read the new XID */
 		g_io_channel_read_line(source, &string, &length, NULL, NULL);
		
		/* Parse the XID */
		new_xid = strtoul(string, NULL, 0);
		g_free(string);

		gtk_main_quit ();
		
		return TRUE;
	}

	if (g_str_equal("print_embedded\n", string) == TRUE)
	{
		/* The command was "print_embedded", so that's what we do */

		g_free(string);		

		return TRUE;
	}

	if (g_str_equal("print_fullpage\n", string) == TRUE)
	{
		/* The command was "print_fullpage", so that's what we do */

		g_free(string);

		/* Not yet implemented */

		/* g_print("3\nXYZ"); */
		
		return TRUE;
	}
	
	/* At last, free the string */
	g_free(string);
	
	DEBUGM("\n");
	return TRUE;
}


/* This function updates the activity bar during the time the file is
 * downloaded. It's called by a glib timeout.
 */
gboolean timeout_func(gpointer data)
{
	static int p = 0;

	/* The progress bar is not shown any longer, return FALSE
	   so the timeout won't be called again. */
	if (timeout_stop) return FALSE;
	
	/* Set some value */
	gtk_progress_set_value(GTK_PROGRESS(progress), p / 1000.0f);
	
	p = (p + 50) % 1000;

	/* Call this again please */
	return TRUE;
}


gboolean my_key_press_event  (GtkWidget   *widget,
                               GdkEventKey *event,
                               gpointer     user_data)
{
//   g_print ("key_press event occurred : %d\n", event->keyval);//65361 left, 65363 right
	gboolean doit=FALSE;
//	
	/* <- left key pressed */
	if (event->keyval == 65361)
	{
	   if ( current_page > 0)
	   {
	      current_page--;
	      current_page_filename(current_page);
	      doit = TRUE;
	   }
	}	
	else 	/* -> right key pressed */
	if (event->keyval == 65363)
	{
	   if ( current_page < (tiff_pages - 1))
	   {
	      current_page++;
	      current_page_filename(current_page);
	      doit = TRUE;
	   }
	}	
	else /* + pressed */
	if (event->keyval == 65451)
	{
		imgwidth += 100;
		imgheight += 100;
		g_object_unref(pixbuf);
		pixbuf = gdk_pixbuf_scale_simple( original_image, imgwidth, imgheight, GDK_INTERP_BILINEAR );
		doit = TRUE;

	}
	else /* - pressed */
	if (event->keyval == 65453)
	{
		imgwidth -= 100;
		imgheight -= 100;
		g_object_unref(pixbuf);
		pixbuf = gdk_pixbuf_scale_simple( original_image, imgwidth, imgheight, GDK_INTERP_BILINEAR );
		doit = TRUE;
		
	}
	else
	if (event->keyval == 97)
	{
	   pixbuf = gdk_pixbuf_rotate_simple( pixbuf, GDK_PIXBUF_ROTATE_COUNTERCLOCKWISE);
	   doit = TRUE;
	   
	}
	else
	if (event->keyval == 99)
	{
		pixbuf = gdk_pixbuf_rotate_simple( pixbuf, GDK_PIXBUF_ROTATE_CLOCKWISE);
		doit = TRUE;
	}

	if (doit)
	{
		control = gtk_image_new_from_pixbuf(pixbuf);
		setMainControl(control);
	}
		
}




/* Main program entry point.
 */
int main(int argc, char * argv[])
{

	/* The XID of the socket we should plug into */
	unsigned long xid;
	
	GError *err = NULL;
	
	gdk_parent = NULL;
	control = NULL;


	width=0;
	height=0;
	imgwidth=400;
	imgheight=300;

	
	/* See if the arguments are somewhat correct */
	if( argc <= 1 )
	{ 
		fprintf(stderr, "%s: not enough args\n", argv[0] );
		fprintf(stderr, "Usage: %s <url> <mime type> [xid]\n", argv[0]);
		exit(1);
	}

	/* Set the url */
	url = argv[1];

	/* Set the Mime type */
	mime_type = argv[2];
	
    gtk_init (&argc, &argv);

	if (argc == 4)
	{
		/* This is embedded mode */
    
		/* Parse the XID */
		xid = strtoul(argv[3], NULL, 0);
    
		plug = createPlug(xid);
		
		embedded = TRUE;
	} else {
		
		/* This is standalone mode */
		plug = gtk_window_new(GTK_WINDOW_TOPLEVEL);
		embedded = FALSE;
	}

	/* Connect some standard signals, so we exit cleanly */
	gtk_signal_connect(GTK_OBJECT(plug), "delete_event", GTK_SIGNAL_FUNC (window_destroyed), NULL);
	gtk_signal_connect(GTK_OBJECT(plug), "destroy", GTK_SIGNAL_FUNC(window_destroyed), NULL);
	gtk_signal_connect(GTK_OBJECT(plug), "key_press_event",  GTK_SIGNAL_FUNC(my_key_press_event), NULL);
		
	/* In embedded mode, prepare the progress bar, and align it */

	if (embedded)
	{
		GtkWidget *vbox, *label, *alignment;
		char message[1024];
		memset(message, 0, 1024);

		width=w_width;
		height=w_height;

		widget = gtk_alignment_new(0.5f, 0.5f, 0.0f, 0.0f);
		
		alignment = gtk_alignment_new(0.5f, 0.5f, 0.0f, 0.0f);
		
		vbox = gtk_vbox_new(FALSE, 12);
		
		gtk_container_add(GTK_CONTAINER(widget), GTK_WIDGET(vbox));
				
		g_snprintf(message, 1023,("Doing %s"), argv[1]);
		
		label = gtk_label_new(message);
		gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
		gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
		gtk_container_add(GTK_CONTAINER(vbox), GTK_WIDGET(label));
		
		progress = gtk_progress_bar_new();
		
		gtk_progress_set_activity_mode(GTK_PROGRESS(progress), TRUE);
		
		gtk_container_add(GTK_CONTAINER(alignment), GTK_WIDGET(progress));
		gtk_container_add(GTK_CONTAINER(vbox), GTK_WIDGET(alignment));
 
		/* Set the timeout (20x per second), so that the activity
		   bar gets updated every once in a while. */
		timeout = g_timeout_add(50, timeout_func, 0);


	}
	else
	{
	   tiff_pages = split_multi_tiff(url);
		original_image = gdk_pixbuf_new_from_file(url , NULL);
		
		pixbuf = gdk_pixbuf_new_from_file_at_scale(url, imgwidth, imgheight, TRUE,NULL);
		widget=gtk_image_new_from_pixbuf(pixbuf);
		
	}

	viewerHAdjustment=gtk_adjustment_new(0,0,100,1.0,1.0,1.0);
	viewerVAdjustment=gtk_adjustment_new(0,0,100,1.0,1.0,1.0);

	scrolledWindow=gtk_scrolled_window_new(GTK_ADJUSTMENT(viewerHAdjustment),GTK_ADJUSTMENT(viewerVAdjustment));

	gtk_widget_set_usize(GTK_WIDGET(scrolledWindow),imgwidth,imgheight);
	
	gtk_widget_show(scrolledWindow);

	
	fixed = gtk_fixed_new();
	
	/* Whatever widget we constructed before, add and display it now */
	
	gtk_widget_set_usize(GTK_WIDGET(fixed), 0, 0 );
	//	gtk_widget_set_usize(GTK_WIDGET(fixed), width, height);
	
	//	gtk_container_add(GTK_CONTAINER(plug), GTK_WIDGET(fixed));
	gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scrolledWindow),fixed);
	
	gtk_container_add (GTK_CONTAINER (plug), scrolledWindow);
	setMainControl(widget);

	if (!embedded)
	{
		
		gtk_widget_set_usize(GTK_WIDGET(plug), 800, 600);
	}
	
	gtk_widget_show_all(GTK_WIDGET(plug));		


	/* Connect the io channel through which we'll get commands */
	in_channel = g_io_channel_unix_new(fileno(stdin));	/* std in */
	g_io_add_watch(in_channel, G_IO_IN, io_func, &err);
	out_channel = g_io_channel_unix_new(fileno(stdout));	/* std in */
	
	if (err != NULL) {
		g_error("Error: %s\n", err->message);
		g_error_free(err);
	}


    /* All GTK applications must have a gtk_main(). Control ends here
     * and waits for an event to occur (like a key press or
     * mouse event). */
	
    gtk_main ();


	return 0;
}
