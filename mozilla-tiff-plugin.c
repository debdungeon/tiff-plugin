/*
 * Mozilla-tiff plugin
 *
 * mozilla-tiff-plugin.c
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

#define VERSION "0.1"

#include <npapi.h>
#include <npupp.h>

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/types.h>

/* Function for printing debugging messages */
void DEBUGM(const char* format, ...)
{
#ifdef DEBUG
	va_list args;
	va_start(args, format);	
	fprintf(stderr, format, args);
	va_end(args);
#endif
}

/* The instance "object" */
typedef struct _PluginInstance
{
	
	char *url;	/* The URL that this instance displays */
	
	char *mime_type;	/* The mime type that this instance displays */
	
	int width, height;	/* The size of the display area */
	
	unsigned long moz_xid;	/* The XID of the window mozilla has for us */
	
	pid_t child_pid;	/* pid of the spawned viewer */
	
	int to_pipe;	/* The pipe connected to the viewers' standard in */
	int from_pipe;	/* The pipe connected to the viewers' standard out */

	FILE *to_stream;	/* The stream connected to the viewers' standard in */
	FILE *from_stream;	/* The stream connected to the viewers' standard out */

	int argc;	
	char **args;
	pthread_t thread;
	NPP instance;
} PluginInstance;

/* Forward declaration of the spawning function */
void spawn_viewer(PluginInstance* This);

/* Forward declaration of the generic spawning function */
void spawn_program(char *argv[], int *to_pipe, int *from_pipe, pid_t *pid);

/* This caches the mime info, should the browser ask for it again */
static char *mime_info_cache = NULL;

/* This function spawns a program and writes a file-descriptor
 * connected to its stdin into "to_pipe", and one connected to
 * its stdout into "from_pipe"
 */
void spawn_program(char *argv[], int *to_pipe, int *from_pipe, pid_t *p_pid)
{

	int pipe1[2];
	int pipe2[2];

	pid_t pid;
	
	/* Create the pipes */
	if (pipe(pipe1) < 0)
	{
		perror("pipe1");
	}
	
	if (pipe(pipe2) < 0)
	{
		perror("pipe2");
	}

	/* Fork */
	
	if ((pid = fork()) < 0)
	{
		perror("fork");
	} else if (pid > 0)
	{
		/* Parent process */
		if (p_pid != NULL) *p_pid = pid;
		
		/* Close pipe ends */
		close(pipe1[0]);
		close(pipe2[1]);
		
		/* Save pipes in This */
		*to_pipe = pipe1[1];
		*from_pipe = pipe2[0];
	}
	else
	{
		/* Child process */

		/* Close pipe ends */
		close(pipe1[1]);
		close(pipe2[0]);

		/* Connect stdin to pipe */
		if (pipe1[0] != STDIN_FILENO){
			if (dup2(pipe1[0], STDIN_FILENO) != STDIN_FILENO)
			{
				perror("dup2(stdin)");
			}
			close(pipe1[0]);
		}

		/* Connect stdout to pipe */
		if (pipe2[1] != STDOUT_FILENO){
			if (dup2(pipe2[1], STDOUT_FILENO) != STDOUT_FILENO)
			{
				perror("dup2(stdout)");
			}
			close(pipe2[1]);
		}
		
		/* Exec the viewer */
		if (execvp(argv[0], argv) < 0)
		{
			perror("execvp");
		}
	}
}

/* The browser calls this to query which mime types are supported.
 * The plugin actually spawns the viewer to get this info, because
 * it can't use bonobo or GConf itself.
 */
char* NPP_GetMIMEDescription()
{

	char *argv[3];
	int argc = 0;
	int from_pipe;
	int to_pipe;
	pid_t pid;

	FILE *stream;
	
	DEBUGM("plugin: NPP_GetMIMEDescription\n");
	
	/* Return the cache if something's in it */
	if (mime_info_cache != NULL) return mime_info_cache;

	mime_info_cache = malloc(16000);
	strncpy(mime_info_cache, "image/tiff::TIFF image (using EOG Image);", 15999);
	
	
	/* Away it goes */
	return mime_info_cache;
}

/* The browser calls this to get the plugin name and description
 */
NPError NPP_GetValue(NPP instance, NPPVariable variable, void *value)
{

	NPError err = NPERR_NO_ERROR;

	DEBUGM("plugin: NPP_GetValue\n");
	
	switch (variable)
	{
		case NPPVpluginNameString:
			/* Return the name */
			*((char **)value) = "Mozilla-tiff-plugin " VERSION;
			break;
		case NPPVpluginDescriptionString:
			/* Return the description */

			*((char **)value) = "<A href=\"http://www.vinay.in/mozilla-tiff-plugin/\">This plugin</A> uses GTK+ to "
					    "display tiff images inline";
		
			break;
		default:
			err = NPERR_GENERIC_ERROR;
	}
	return err;
}

/* This is called to initialise the plugin
 */
NPError NPP_Initialize() {
	DEBUGM("plugin: NPP_Initialise\n");

	return NPERR_NO_ERROR;
}

/* This is called to create a new instance of the plugin
 */
NPError NPP_New(NPMIMEType pluginType, NPP instance, uint16 mode, int16 argc,
		char* argn[], char* argv[], NPSavedData* saved)
{
			
	PluginInstance* This;
	int i, j;

	DEBUGM("plugin: NPP_New\n");

	if (instance == NULL)
	{
		return NPERR_INVALID_INSTANCE_ERROR;
	}

	instance->pdata = NPN_MemAlloc(sizeof(PluginInstance));

	This = (PluginInstance*) instance->pdata;
	This->instance = instance;

	if (This == NULL)
	{
		return NPERR_OUT_OF_MEMORY_ERROR;
	}

	/* Initialise some values */
	This->to_pipe = 0;
	This->from_pipe = 0;
	
	This->moz_xid = 0;
	This->url = NULL;

	This->mime_type = strdup(pluginType);
	
	This->argc = argc;
	This->args = malloc (argc * 2 * sizeof (char*));
	for (i = j = 0; i < argc; i++)
	{

		if (!strcmp(argn[i], "PARAM"))
		{
			i++;
			This->argc--;
		}

		This->args[j] = malloc (strlen (argn[i]) + 1);
		strcpy (This->args[j++], argn[i]);
		This->args[j] = malloc (strlen (argv[i]) + 1);
		strcpy (This->args[j++], argv[i]);
	}
	
	return NPERR_NO_ERROR;
}

/* This destroys a plugin instance
 */
NPError NPP_Destroy(NPP instance, NPSavedData** save)
{

	PluginInstance* This;
	int i, status;
	void *returned_value;

	DEBUGM("plugin: NPP_Destroy\n");
	
	if (instance == NULL)
	{
		return NPERR_INVALID_INSTANCE_ERROR;
	}

	This = (PluginInstance*) instance->pdata;

	if (This != NULL)
	{

		if (This->to_pipe != 0)
		{
			/* If the viewer has actually been spawned */
	
			/* Send it the exit command */
			fprintf(This->to_stream, "exit\n");

			/* The flush is important */
			fflush(This->to_stream);
			
			pthread_join (This->thread, &returned_value);

			/* Wait for the process to end so no zombie process is left */
			if (waitpid(This->child_pid, &status, 0) < 0) perror("waitpid");
		}

		if (This->from_pipe != 0)
		{
			fclose (This->from_stream);
		}
		
		/* Free some resources */
		if (This->url) free(This->url);

		if (This->args)
		{
			for (i = 0; i < This->argc * 2; i++) free (This->args[i]);
			free (This->args);
		}

		free(This->mime_type);
		
		NPN_MemFree(instance->pdata);
		This = NULL;
		instance->pdata = NULL;
	}

	return NPERR_NO_ERROR;
}

/* The listening thread*/
void* listen_viewer (void* arg)
{
	char buf[256];
	static int status = 0 ;
	PluginInstance* This = (PluginInstance *) arg;
	
	while (fgets (buf, sizeof (buf), This->from_stream))
	{
		buf [strlen (buf) -1] = 0;
		if (!strcmp (buf, "URL"))
		{
			fgets (buf, sizeof (buf), This->from_stream);
			buf [strlen (buf) -1] = 0;
			NPN_GetURL (This->instance, buf, "_self");
		}
		else if (!strcmp (buf, "exit"))
			break;
	}
	pthread_exit(&status) ;
}

/* This function actually spawns the viewer. It expects the "This->url"
 * field and the "This->moz_xid" field to be valid.
 */
void spawn_viewer(PluginInstance* This)
{

	char *argv[5];
	int argc = 0;
	
	char xid_str[32];
	
	/* Construct the command line */
	snprintf(xid_str, 31, "%ld", This->moz_xid);
	
	argv[argc++] = "mozilla-tiff-viewer";
	argv[argc++] = This->url;
	argv[argc++] = This->mime_type;
	argv[argc++] = xid_str;
	argv[argc++] = NULL;

	spawn_program(argv, &This->to_pipe, &This->from_pipe, &This->child_pid);
	
	This->to_stream = (FILE*)fdopen(This->to_pipe, "w");
	This->from_stream = (FILE*)fdopen(This->from_pipe, "r");

	/*Start a new thread to listen This->from_stream*/
	pthread_create (&This->thread, NULL, listen_viewer, This);
}

/* This is tricky stuff. Mozilla calls it several times with different
 * parameters. Basically it tells us the window into which we should draw,
 * and tells us when the size changes or when the window is reparented.
 */
NPError NPP_SetWindow(NPP instance, NPWindow* window)
{

	PluginInstance* This;

	DEBUGM("plugin: NPP_SetWindow");

	if (instance == NULL)
	{
		return NPERR_INVALID_INSTANCE_ERROR;
	}

	This = (PluginInstance*) instance->pdata;
	
	if ((window == NULL) || (window->window == NULL))
	{
		DEBUGM(" null window\n");
		return NPERR_NO_ERROR;
	}
	
	if (This->moz_xid){
		/* The window exists already */
		if(This->moz_xid == (unsigned long) window->window)
		{
			/* The window is the same as it was for the last call */
			if (This->to_pipe != 0)
			{
				/* The viewer is actually spawned, so this is basically
				   a resize event. We need to notify the viewer of the
				   new size. */
				
				/* Get the new width/height */
				int width = window->width;
				int height = window->height;
				
				DEBUGM(" resize");
				
				/* Send them over */
				fprintf(This->to_stream, "size\n%i\n%i\n", width, height);
				
				/* Flush it */
				fflush(This->to_stream);
			} else {
				DEBUGM(" viewer not yet spawned");
				/* The viewer is not yet spawned. Don't do anything then. */
			}
		}
		else
		{
			DEBUGM(" parent changed!");
			/* The parent window changed. Reparent accordingly. */
			This->moz_xid = (unsigned long) window->window;		
			
			fprintf(This->to_stream, "reparent\n%ld\n", This->moz_xid);
			
			fflush(This->to_stream);
		}
        } else {
		DEBUGM(" init");
		/* This is actually the first time we're given a window to draw into */
		
		/* Get its XID and save it for later */
		This->moz_xid = (unsigned long) window->window;		
	}
	
	DEBUGM("\n");
	return NPERR_NO_ERROR;
}

/* This is how the browser passes us a stream. It's also used to tell the
 * browser "we don't want it", and "go download it yourself".
 * TODO: Sometime in the future, this should stream the data if the
 * bonobo control supports PersistStream.
 */
NPError NPP_NewStream(NPP instance, NPMIMEType type, NPStream *stream,
		      NPBool seekable, uint16 *stype)
{

	PluginInstance* This;
			      
	DEBUGM("plugin: NPP_NewStream\n");
			      
	This = (PluginInstance*) instance->pdata;

	/* Save the url, so the viewer can display it. */
	This->url = (char*)strdup(stream->url);
			      
	/* By setting this we ask mozilla to download the file for us,
	   to save it in its cache, and to tell us when it's done */
	*stype = NP_ASFILEONLY;
	
	/* Now we can spawn the viewer, which will display something nice
	   for us to stare at while the download trickles on... */
	spawn_viewer(This);
	
			      
	return NPERR_NO_ERROR;
}

/* This function is called by the browser once the file download is
 * completed. It passes us the filename, and we pass it on to the
 * viewer so that it can load and display a matching control.
 */
void NPP_StreamAsFile(NPP instance, NPStream *stream, const char* fname)
{

	PluginInstance* This;
	int i, j;

	if (instance == NULL) return;
	
	This = (PluginInstance*) instance->pdata;

	if (fname != NULL)
	{
	
		DEBUGM("plugin: NPP_StreamAsFile(%s)\n", strdup(fname));
		
		/* Tell the viewer the filename of the downloaded file. */
		fprintf(This->to_stream, "filename\nfile://%s\n", fname);

		/* Send the arguments to the viewer. */
		for (i = j = 0; i < This->argc; i++, j+=2)
			fprintf (This->to_stream, "param\n%s\n%s\n", This->args[j], This->args[j + 1]);
		
		/* Flush */
		fflush(This->to_stream);
	}
	
	return;
}

/* The functions below are not needed / implemented */
void NPP_URLNotify(NPP instance, const char* url, NPReason reason, void* notifyData) {

	DEBUGM("plugin: NPP_URLNotify(%s)\n", url);
}

void NPP_Shutdown(void)
{
	
	DEBUGM("plugin: NPP_Shutdown\n");
}

void NPP_Print(NPP instance, NPPrint* printInfo)
{

	PluginInstance* This;
	
	if (printInfo == NULL)
	{
		DEBUGM("plugin: NPP_Print(): printinfo == NULL\n");
		return;
	}

	if (instance == NULL)
	{
		DEBUGM("plugin: NPP_Print(): instance == NULL\n");
		return;
	}
		
	This = (PluginInstance*) instance->pdata;
	
	if (printInfo->mode == NP_FULL)
	{
		/* In Unix, use platformPrint to get pointer to postscript file
		   to write postcript data. */

		char *platformPrint = printInfo->print.fullPrint.platformPrint;
	
		/* If pluginPrinted is set to FALSE, NPP_Print SHOULD be
		   called again, with mode = NP_EMBED
		   If set to TRUE, it should not be called again. */
		printInfo->print.fullPrint.pluginPrinted = FALSE;
		
		DEBUGM("plugin: NPP_Print(NP_FULL, %s)\n", platformPrint);
	}
	else
	{
		char ps_size_s[32];
		int ps_size;
		char *ps_data;
		
		/* NPWindow* printWindow = &(printInfo->print.embedPrint.window); */
		
		NPPrintCallbackStruct *platformPrint = printInfo->print.embedPrint.platformPrint;
		/* fprintf(platformPrint->fp, "%s", "(mozilla-bonobo prints!) show"); */
		
		DEBUGM("plugin: NPP_Print(NP_EMBED, %x)\n", platformPrint);
		
		fprintf(This->to_stream, "print_embedded\n");
		fflush(This->to_stream);
		
		fgets(ps_size_s, 32, This->from_stream);
		ps_size = atoi(ps_size_s);
		
		ps_data = malloc(ps_size + 1);
		memset(ps_data, 0, ps_size + 1);
		
		fread(ps_data, 1, ps_size, This->from_stream);
		fwrite(ps_data, 1, ps_size, platformPrint->fp);
		
		free(ps_data);
	}
	
	return;
}

int32 NPP_WriteReady(NPP instance, NPStream *stream) {
	
	DEBUGM("plugin: NPP_WriteReady\n");
	return 0;
}


int32 NPP_Write(NPP instance, NPStream *stream, int32 offset, int32 len, void *buffer)
{

	DEBUGM("plugin: NPP_Write\n");
	return 0;
}


NPError NPP_DestroyStream(NPP instance, NPStream *stream, NPError reason)
{

	DEBUGM("plugin: NPP_DestroyStream\n");
	return NPERR_NO_ERROR;
}

jref NPP_GetJavaClass()
{
	return NULL;
}
